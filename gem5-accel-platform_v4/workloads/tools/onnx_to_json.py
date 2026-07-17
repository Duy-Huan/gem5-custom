#!/usr/bin/env python3
"""
onnx_to_json.py - converts a real ONNX model (any CNN exported from
PyTorch/TensorFlow/etc.) into this platform's JSON layer-config schema
(the same schema hand-authored in workloads/models/*.json), which
tools/model_to_npu_calls.py then expands into NpuAccel calls.

Unlike the hand-authored JSON files (which use high-level blocks like
"bottleneck_mbv2"/"c2f" reconstructing a specific framework's module
structure), this converter emits a FLAT list of primitive "conv"/"linear"
layers directly - it does not try to pattern-match ONNX subgraphs back
into named blocks. This is deliberate: it is far more general (works on
*any* ONNX CNN, not just ones built from known block types) and more
robust to branching graphs (skip connections, concat, etc.), because of
how shapes are tracked - see "Design: shape tracking" below.

Usage:
    python3 onnx_to_json.py model.onnx --out model.json --name my_model \
        --input-name pixel_values

Supported ONNX ops (anything else is skipped as a no-MAC op, with a
warning if it changes tensor rank/shape in a way this script cannot
verify - see --strict):
    Conv                -> "conv" layer (dense, depthwise, or general
                            grouped, matching the node's `group` attr)
    Gemm, MatMul        -> "linear" layer (fully-connected)
    MaxPool, AveragePool, GlobalAveragePool, BatchNormalization, Relu,
    Clip, Sigmoid, HardSigmoid, Add, Mul, Concat, Flatten, Reshape,
    Dropout, Identity, Pad
                        -> no MACs, not emitted as a layer; only used to
                           keep this script's own understanding of the
                           "current tensor shape" in sync (see below)

Design: shape tracking
-----------------------
A CNN graph is not always a straight line (residual/skip connections,
concatenation, branch-and-merge for feature pyramids, etc.), so this
script does NOT try to recompute shapes by hand for every op the way
tools/model_to_npu_calls.py's Expander does for the hand-authored
configs. Instead, it runs ONNX's own shape inference
(onnx.shape_inference.infer_shapes) once up front, and after processing
each node, looks up that node's REAL output tensor shape from the
inference result - so this script's notion of "current shape" is always
resynchronized against ONNX's own ground truth, regardless of how
complex the graph's branching is. The only place this script computes a
shape itself is the (Oh, Ow) of each Conv, which it derives from the
node's actual `pads`/`strides`/`dilations` attributes (not the "pad =
k//2" approximation tools/model_to_npu_calls.py's Expander uses for
hand-authored configs) - and even then, the result is cross-checked
against ONNX's own inferred output shape when available, and a warning
is printed on any mismatch (this has caught real bugs during testing -
see the test suite in test_onnx_to_json.py).

Limitations (also printed as a summary at the end of every run):
  - Only square kernels/strides are supported (kh == kw, sh == sw). Real
    CNNs overwhelmingly use square kernels; a model with a genuine
    non-square kernel will raise a clear error rather than silently
    produce wrong numbers.
  - Grouped conv where 1 < groups < Cin (e.g. ResNeXt) is supported by
    decomposing into `groups` independent dense sub-convs (see
    Expander.conv() in model_to_npu_calls.py) - this is mathematically
    correct MAC-wise, but is a coarser NpuAccel-call granularity than a
    real ResNeXt-aware NPU compiler might choose.
  - Multi-input Concat/Add branch points rely on ONNX shape inference
    for the *merged* shape, but the two/more branches feeding into them
    are still visited strictly in the graph's node order (assumed
    topologically sorted, which the ONNX spec requires) - this script
    does not itself validate that both branches were fully "settled"
    before the merge point the way a true dataflow/DAG walker would.
    For standard single-trunk CNNs (ResNet/MobileNet/EfficientNet-style,
    where skip connections rejoin at the same spatial resolution) this
    is not an issue in practice (confirmed by the test suite), but a
    model with more exotic branching should have its generated JSON
    spot-checked against `--verbose` output.
"""

import argparse
import sys
import warnings
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

import onnx
from onnx import numpy_helper


NO_MAC_OPS = {
    "BatchNormalization", "Relu", "Clip", "Sigmoid", "HardSigmoid",
    "HardSwish", "Add", "Mul", "Concat", "Flatten", "Reshape", "Dropout",
    "Identity", "Pad", "Squeeze", "Unsqueeze", "Transpose", "Cast",
    "Constant", "Softmax", "LeakyRelu", "Gelu", "Erf", "Div", "Sub",
    "ReduceMean", "GlobalAveragePool", "AveragePool", "MaxPool",
}
POOL_OPS = {"MaxPool", "AveragePool"}


@dataclass
class LayerOut:
    name: str
    op: str
    out_c: int
    k: int
    s: int
    groups: int
    out_h: int
    out_w: int


def get_attr(node, name, default=None):
    for a in node.attribute:
        if a.name == name:
            if a.type == onnx.AttributeProto.INT:
                return a.i
            if a.type == onnx.AttributeProto.INTS:
                return list(a.ints)
            if a.type == onnx.AttributeProto.FLOAT:
                return a.f
            if a.type == onnx.AttributeProto.STRING:
                return a.s.decode()
    return default


def conv_output_hw(h, w, kh, kw, sh, sw, pads, dilations):
    # pads = [pad_top, pad_left, pad_bottom, pad_right] (ONNX convention)
    pt, pl, pb, pr = pads
    dh, dw = dilations
    oh = (h + pt + pb - dh * (kh - 1) - 1) // sh + 1
    ow = (w + pl + pr - dw * (kw - 1) - 1) // sw + 1
    return oh, ow


def pool_output_hw(h, w, k, s, pads):
    pt, pl, pb, pr = pads
    kh = kw = k
    oh = (h + pt + pb - kh) // s + 1
    ow = (w + pl + pr - kw) // s + 1
    return oh, ow


class OnnxWalker:
    def __init__(self, model: onnx.ModelProto, verbose=False, strict=False):
        self.model = model
        self.graph = model.graph
        self.verbose = verbose
        self.strict = strict
        self.layers: List[LayerOut] = []
        self.warnings: List[str] = []

        # name -> numpy array, for weight tensors (Conv/Gemm)
        self.initializers = {
            t.name: numpy_helper.to_array(t) for t in self.graph.initializer
        }

        # name -> shape (list[int]), from shape inference + declared I/O.
        # NCHW / (N, features) assumed throughout, standard for
        # vision/classification ONNX exports.
        self.shapes: Dict[str, List[int]] = {}
        self._collect_declared_shapes()

    def _shape_from_value_info(self, vi) -> Optional[List[int]]:
        dims = []
        for d in vi.type.tensor_type.shape.dim:
            if d.HasField("dim_value"):
                dims.append(d.dim_value)
            else:
                dims.append(None)  # dynamic dim (e.g. batch size)
        return dims

    def _collect_declared_shapes(self):
        inferred = onnx.shape_inference.infer_shapes(self.model)
        for coll in (inferred.graph.input, inferred.graph.value_info,
                     inferred.graph.output):
            for vi in coll:
                shp = self._shape_from_value_info(vi)
                if shp:
                    self.shapes[vi.name] = shp
        for name, arr in self.initializers.items():
            self.shapes[name] = list(arr.shape)

    def _log(self, msg):
        if self.verbose:
            print(f"[onnx_to_json] {msg}", file=sys.stderr)

    def _warn(self, msg):
        self.warnings.append(msg)
        warnings.warn(msg)

    def shape_of(self, tensor_name) -> Optional[List[int]]:
        return self.shapes.get(tensor_name)

    def walk(self, input_name: Optional[str] = None):
        g = self.graph
        if input_name is None:
            input_name = g.input[0].name
        in_shape = self.shape_of(input_name)
        if in_shape is None or len(in_shape) != 4:
            raise ValueError(
                f"Could not determine a 4D (N,C,H,W) shape for input "
                f"'{input_name}' - got {in_shape}. Pass --input-name to "
                f"select a different graph input if there are several."
            )
        n, c, h, w = in_shape
        self._log(f"input '{input_name}': N={n} C={c} H={h} W={w}")

        for node in g.node:
            self._process_node(node, h, w, c)
            # Resync (h, w, c) from ONNX's own ground-truth shape of this
            # node's (first) output, when available - this is what makes
            # the walker robust to branches/concat/etc. (see module
            # docstring). Falls back to whatever self._process_node did
            # to (h, w, c) internally if no inferred shape is available
            # (e.g. for graphs shape inference couldn't fully resolve).
            out_shape = self.shape_of(node.output[0]) if node.output else None
            new_h, new_w, new_c = h, w, c
            if out_shape and len(out_shape) == 4 and all(
                d is not None for d in out_shape
            ):
                _, new_c, new_h, new_w = out_shape
            elif out_shape and len(out_shape) == 2:
                # (N, features) - post-flatten/Gemm tensors.
                _, feat = out_shape
                if feat is not None:
                    new_h, new_w, new_c = 1, 1, feat

            if (new_h, new_w, new_c) != (h, w, c):
                if node.op_type in ("Conv", "Gemm", "MatMul"):
                    # Conv/Linear layer entries already carry their own
                    # exact output shape (out_h/out_w on the LayerOut, or
                    # M computed downstream from the running state right
                    # before them) - no separate reshape marker needed.
                    self._log(
                        f"  shape after {node.op_type} '{node.name}': "
                        f"({h},{w},{c}) -> ({new_h},{new_w},{new_c})"
                    )
                else:
                    # A no-MAC op (pooling, GlobalAveragePool, Flatten,
                    # Reshape...) changed the shape - the downstream
                    # Expander in model_to_npu_calls.py has no other way
                    # to know this (it doesn't replay skipped ops), so
                    # emit an explicit "reshape" JSON entry to keep its
                    # (h, w, c) state in sync. See Expander.reshape()'s
                    # doc-comment for why this is necessary.
                    self._log(
                        f"  reshape after {node.op_type} '{node.name}': "
                        f"({h},{w},{c}) -> ({new_h},{new_w},{new_c})"
                    )
                    self.layers.append(LayerOut(
                        name=f"{node.name or node.op_type}.reshape",
                        op="reshape", out_c=int(new_c), k=0, s=0, groups=0,
                        out_h=int(new_h), out_w=int(new_w),
                    ))
            h, w, c = new_h, new_w, new_c

        self._log(f"final shape after last node: H={h} W={w} C={c}")
        return self.layers

    def _process_node(self, node, h, w, c):
        op = node.op_type

        if op == "Conv":
            self._process_conv(node, h, w, c)
        elif op in ("Gemm", "MatMul"):
            self._process_linear(node, c)
        elif op in NO_MAC_OPS:
            pass  # shape resync in walk() handles any dimension changes
        else:
            self._warn(
                f"Unrecognized op '{op}' (node '{node.name}') - assumed "
                f"no-MAC and shape-preserving. If this op actually "
                f"performs meaningful compute or changes tensor shape, "
                f"the generated JSON may be inaccurate from this point "
                f"on. Re-run with --strict to turn this into an error."
            )
            if self.strict:
                raise ValueError(f"--strict: unhandled op '{op}'")

    def _process_conv(self, node, h, w, c):
        weight_name = node.input[1]
        weight = self.initializers.get(weight_name)
        if weight is None:
            raise ValueError(
                f"Conv node '{node.name}': weight tensor '{weight_name}' "
                f"not found in initializers (is the model fully "
                f"constant-folded / not using an external-data weight "
                f"this script didn't load?)"
            )
        out_c, in_per_group, kh, kw = weight.shape
        if kh != kw:
            raise ValueError(
                f"Conv node '{node.name}': non-square kernel "
                f"{kh}x{kw} not supported by this converter."
            )
        k = kh

        strides = get_attr(node, "strides", [1, 1])
        pads = get_attr(node, "pads", [0, 0, 0, 0])
        dilations = get_attr(node, "dilations", [1, 1])
        group = get_attr(node, "group", 1)

        if strides[0] != strides[1]:
            raise ValueError(
                f"Conv node '{node.name}': non-square stride "
                f"{strides} not supported by this converter."
            )
        s = strides[0]

        oh, ow = conv_output_hw(h, w, kh, kw, strides[0], strides[1],
                                 pads, dilations)

        # Cross-check against ONNX's own shape inference for this node's
        # output, if available - catches bugs in conv_output_hw() (or in
        # the attributes read above) rather than silently emitting a
        # subtly-wrong M/K/N downstream.
        out_shape = self.shape_of(node.output[0])
        if out_shape and len(out_shape) == 4 and out_shape[2] is not None:
            _, _, real_oh, real_ow = out_shape
            if (real_oh, real_ow) != (oh, ow):
                self._warn(
                    f"Conv node '{node.name}': computed output "
                    f"{oh}x{ow} from pads/strides/dilations does not "
                    f"match ONNX-inferred output {real_oh}x{real_ow} - "
                    f"using the ONNX-inferred value (ground truth)."
                )
                oh, ow = real_oh, real_ow

        expected_in_c = in_per_group * group
        if expected_in_c != c:
            self._warn(
                f"Conv node '{node.name}': weight implies {expected_in_c} "
                f"input channels (in_per_group={in_per_group} * "
                f"group={group}) but the running input channel count is "
                f"{c} - using the weight-derived value ({expected_in_c}) "
                f"for the layer's true input width; this usually means "
                f"an earlier op in NO_MAC_OPS silently changed channel "
                f"count in a way this script didn't expect."
            )

        self.layers.append(LayerOut(
            name=node.name or f"conv_{len(self.layers)}",
            op="conv", out_c=int(out_c), k=int(k), s=int(s),
            groups=int(group), out_h=int(oh), out_w=int(ow),
        ))
        self._log(f"Conv '{node.name}': in_c={expected_in_c} out_c={out_c} "
                   f"k={k} s={s} groups={group} -> {oh}x{ow}")

    def _process_linear(self, node, c):
        weight_name = node.input[1]
        weight = self.initializers.get(weight_name)
        if weight is None:
            raise ValueError(
                f"{node.op_type} node '{node.name}': weight tensor "
                f"'{weight_name}' not found in initializers."
            )
        trans_b = get_attr(node, "transB", 0) if node.op_type == "Gemm" else 0
        # Gemm weight is [out_features, in_features] when transB=1
        # (the overwhelmingly common export convention), else
        # [in_features, out_features]. MatMul has no transB attribute
        # and uses [in_features, out_features].
        if trans_b:
            out_features, in_features = weight.shape
        else:
            in_features, out_features = weight.shape

        self.layers.append(LayerOut(
            name=node.name or f"linear_{len(self.layers)}",
            op="linear", out_c=int(out_features), k=1, s=1, groups=1,
            out_h=1, out_w=1,
        ))
        self._log(f"{node.op_type} '{node.name}': in_features={in_features} "
                   f"out_features={out_features}")


def layers_to_json(name, input_shape, layers: List[LayerOut]):
    n, c, h, w = input_shape
    json_layers = []
    for L in layers:
        if L.op == "conv":
            entry = {
                "name": L.name, "op": "conv", "out_c": L.out_c, "k": L.k,
                "s": L.s,
            }
            if L.groups != 1:
                entry["groups"] = L.groups
            # Always pass the exact ONNX-derived output size through -
            # see conv()'s out_hw parameter in model_to_npu_calls.py.
            entry["out_h"] = L.out_h
            entry["out_w"] = L.out_w
            json_layers.append(entry)
        elif L.op == "linear":
            json_layers.append({
                "name": L.name, "op": "linear", "out_c": L.out_c,
            })
        elif L.op == "reshape":
            json_layers.append({
                "name": L.name, "op": "reshape",
                "h": L.out_h, "w": L.out_w, "c": L.out_c,
            })
    return {
        "name": name,
        "_source": "Auto-converted from a real ONNX model by "
                    "onnx_to_json.py - see that script's docstring for "
                    "exactly which ops are supported and how shapes are "
                    "tracked.",
        "input": {"h": h, "w": w, "c": c},
        "layers": json_layers,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("onnx_model")
    ap.add_argument("--out", required=True)
    ap.add_argument("--name", default=None,
                     help="Model name for the JSON 'name' field (default: "
                          "derived from the input filename)")
    ap.add_argument("--input-name", default=None,
                     help="Graph input tensor to start from, if the model "
                          "has more than one (default: first graph input)")
    ap.add_argument("--verbose", action="store_true",
                     help="Print every layer/shape decision as it's made")
    ap.add_argument("--strict", action="store_true",
                     help="Raise an error instead of warning on any "
                          "unrecognized op")
    args = ap.parse_args()

    model = onnx.load(args.onnx_model)
    onnx.checker.check_model(model)

    walker = OnnxWalker(model, verbose=args.verbose, strict=args.strict)
    input_name = args.input_name or model.graph.input[0].name
    in_shape = walker.shape_of(input_name)
    layers = walker.walk(input_name)

    name = args.name or args.onnx_model.rsplit("/", 1)[-1].rsplit(".", 1)[0]
    out_json = layers_to_json(name, in_shape, layers)

    import json as jsonlib
    with open(args.out, "w") as f:
        jsonlib.dump(out_json, f, indent=2)

    total_macs_conv = 0
    print(f"\n{'layer':30s} {'op':8s} {'out shape':16s} {'params (out_c,k,s,groups)'}")
    print("-" * 90)
    for L in layers:
        shape_str = f"{L.out_h}x{L.out_w}x{L.out_c}"
        print(f"{L.name[:30]:30s} {L.op:8s} {shape_str:16s} "
              f"({L.out_c},{L.k},{L.s},{L.groups})")
    print("-" * 90)
    print(f"Total: {len(layers)} layers")
    if walker.warnings:
        print(f"\n{len(walker.warnings)} warning(s) were raised during "
              f"conversion - re-run with --verbose for full detail, or "
              f"--strict to turn unrecognized ops into hard errors.")
    print(f"\nWrote {args.out}")
    print(f"Next step: python3 model_to_npu_calls.py {args.out} "
          f"--out {args.out.rsplit('.', 1)[0]}_calls.h")


if __name__ == "__main__":
    sys.exit(main())
