#!/usr/bin/env python3
"""
model_to_npu_calls.py - reads a model description (JSON: input shape + a
sequence of high-level layer blocks) and expands every block into its
real primitive operations, mapped onto the NpuAccel (M, K, N, layer_type)
convention via the standard im2col construction (for CNNs) or standard
GEMM projections (for transformers). Supported high-level blocks:

  CNN:         conv, bottleneck_mbv2 (MobileNet), c2f, sppf (YOLOv8), fc
  Transformer: transformer_layer (self-attention + gated MLP,
               GQA/MQA/MHA-general - see Expander.attention()), linear
               (a standalone projection, e.g. an LM head)

This mirrors what the actual framework graph looks like - e.g. a C2f
block becomes its 1x1 + n*(3x3+3x3) + 1x1 convs, a transformer_layer
becomes q/k/v/o projections + per-head QK^T/softmax-V + gate/up/down MLP
projections - and emits:

  1. A human-readable per-layer summary to stdout (name, output shape,
     theoretical MACs, number of NpuAccel calls needed).
  2. A ready-to-compile C header with a static array of NpuAccel calls
     that a workloads/npu_*_bench.c loops over.

Usage:
    python3 tools/model_to_npu_calls.py models/mobilenetv2.json \
        --out models/mobilenetv2_calls.h
    python3 tools/model_to_npu_calls.py models/gemma_2b.json \
        --out models/gemma_2b_calls.h

Key modeling choices (documented here so they're easy to find/adjust):
  - Padding is assumed "same-ish": pad = k // 2, output size
    floor((in + 2*pad - k) / s) + 1. This matches how YOLOv8/MobileNet
    conv layers are actually configured (odd kernels, pad=k//2).
  - SPPF's maxpool operations are NOT sent to the NPU (pooling has no
    MACs and would typically run on a separate unit or the CPU) - only
    its two 1x1 convs are modeled. This is called out explicitly in the
    generated summary.
  - A depthwise conv layer with Cin input channels becomes Cin separate
    NpuAccel calls (M=Oh*Ow, K=Kh*Kw, N=1, layer_type=DEPTHWISE) because
    it is not a single dense GEMM - see the doc-comment in npu_accel.hh
    for why. This is by far the biggest driver of "realistic but
    expensive" simulation time for MobileNet-style models; see
    --depthwise-channel-batch below if you need to approximate faster.
  - For transformer_layer, each attention head's QK^T and softmax-V
    matmuls are issued as their own dense GEMM call (heads don't share a
    reduction dimension with each other), same spirit as the depthwise
    split above but still fully dense per call - see
    Expander.attention()'s doc-comment. RMSNorm/LayerNorm and the
    softmax/gate activations have no MACs and are not modeled.
  - The model JSON's "input" field doubles as (seq_len, 1, hidden_size)
    for transformer models - there's no separate schema for it, "h" just
    means "token count" instead of "image rows" in that context.
  - Large layers (M = Oh*Ow, or M=seq_len for transformers, beyond
    --max-m-per-call) are split into multiple calls tiled along M. This
    models a real NPU compiler tiling a layer to fit scratchpad/PARAM-
    field limits (PARAM0 packs M/K/N as 16-bit fields each, so M/K/N
    must individually stay <= 65535 too - the script checks this and
    errors out if a single tile still overflows, which would mean
    --max-m-per-call needs lowering or the layer's K/N needs its own
    tiling, not implemented here).
"""

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from pathlib import Path

LAYER_TYPE_DENSE = 0
LAYER_TYPE_DEPTHWISE = 1

ACCEL_ID_NPU = 0


@dataclass
class PrimOp:
    name: str
    kind: str  # "dense" or "depthwise"
    oh: int
    ow: int
    cin: int
    cout: int
    kh: int
    kw: int


@dataclass
class NpuCall:
    label: str
    m: int
    k: int
    n: int
    layer_type: int
    len_bytes: int


def conv_out_hw(h, w, k, s):
    pad = k // 2
    oh = (h + 2 * pad - k) // s + 1
    ow = (w + 2 * pad - k) // s + 1
    return oh, ow


class Expander:
    """Walks the (H, W, C) 'current tensor shape' through the layer list,
    yielding PrimOp for every primitive op encountered."""

    def __init__(self, h, w, c):
        self.h, self.w, self.c = h, w, c
        self.ops = []

    def _dense(self, name, m, k, n):
        """Low-level: append a dense GEMM op with explicit (M, K, N),
        bypassing the spatial conv_out_hw() formula - used for
        transformer projections and attention, where 'M' is a token
        count rather than a real conv output size. kh=kw=1 makes
        prim_op_to_calls() compute K = 1*1*cin = k exactly, N = n."""
        self.ops.append(PrimOp(name, "dense", m, 1, k, n, 1, 1))

    def conv(self, name, out_c, k, s, depthwise=False):
        oh, ow = conv_out_hw(self.h, self.w, k, s)
        if depthwise:
            assert out_c == self.c, "depthwise conv must preserve channels"
            self.ops.append(
                PrimOp(name, "depthwise", oh, ow, self.c, out_c, k, k)
            )
        else:
            self.ops.append(
                PrimOp(name, "dense", oh, ow, self.c, out_c, k, k)
            )
        self.h, self.w, self.c = oh, ow, out_c

    def fc(self, name, out_features):
        # Treated as a dense GEMM with a single spatial position.
        self.ops.append(PrimOp(name, "dense", 1, 1, self.c, out_features, 1, 1))
        self.c = out_features

    def linear(self, name, out_features):
        """Dense projection over the current (seq_len, hidden) state -
        the transformer analog of a 1x1 conv: M=seq_len is preserved,
        channels go from hidden -> out_features. Used for LM head /
        standalone projections; attention()/mlp_gated() below build
        their own calls directly with _dense() since Q/K/V branch off
        the *same* input rather than chaining."""
        m = self.h * self.w
        self._dense(name, m, self.c, out_features)
        self.c = out_features

    def attention(self, name, num_heads, num_kv_heads, head_dim,
                  hidden_out=None):
        """One transformer self-attention block (GQA-general - set
        num_kv_heads == num_heads for plain multi-head attention, or
        num_kv_heads == 1 for multi-query attention, matching Gemma's
        MHA/MQA/GQA variants across model sizes).

        Q/K/V/O are all dense GEMM projections (standard im2col-free
        linear layer: M=seq_len, K=hidden_in, N=proj_dim).

        The actual scaled-dot-product attention (QK^T then times V) is
        computed *per head*: heads don't share a reduction dimension
        with each other, so - much like depthwise conv's per-channel
        split - each head is issued as its own dense GEMM call. Unlike
        depthwise, though, each per-head call is still a *full* dense
        GEMM (the reduction over head_dim is complete, nothing is left
        out) - it's just that head_dim is usually small (~256 here),
        so these calls tend to be K-light relative to M=N=seq_len. This
        is what lets the model show attention's actual cost profile:
        cheap per head at short seq_len, but scaling with seq_len^2
        (unlike the linear projections, which scale with seq_len^1).
        """
        seq_len = self.h * self.w
        hidden_in = self.c
        hidden_out = hidden_in if hidden_out is None else hidden_out

        self._dense(f"{name}.q_proj", seq_len, hidden_in,
                    num_heads * head_dim)
        self._dense(f"{name}.k_proj", seq_len, hidden_in,
                    num_kv_heads * head_dim)
        self._dense(f"{name}.v_proj", seq_len, hidden_in,
                    num_kv_heads * head_dim)

        for h in range(num_heads):
            # scores = Q_h @ K_h^T   (M=seq_len, K=head_dim, N=seq_len)
            self._dense(f"{name}.h{h}.qk", seq_len, head_dim, seq_len)
            # out_h  = softmax(scores) @ V_h (M=seq_len, K=seq_len, N=head_dim)
            # (softmax itself has no MACs and isn't sent to the NPU.)
            self._dense(f"{name}.h{h}.av", seq_len, seq_len, head_dim)

        self._dense(f"{name}.o_proj", seq_len, num_heads * head_dim,
                    hidden_out)
        self.c = hidden_out

    def mlp_gated(self, name, intermediate):
        """Gated MLP (SwiGLU/GeGLU - Gemma, Llama, etc. all use this
        shape): gate_proj and up_proj both branch from the same hidden
        input; their element-wise gate-activation-and-multiply has no
        MACs and isn't sent to the NPU; down_proj projects back down.
        Channel count in/out is unchanged (residual-style block)."""
        seq_len = self.h * self.w
        hidden = self.c
        self._dense(f"{name}.gate_proj", seq_len, hidden, intermediate)
        self._dense(f"{name}.up_proj", seq_len, hidden, intermediate)
        self._dense(f"{name}.down_proj", seq_len, intermediate, hidden)
        # self.c unchanged - down_proj restores the original hidden size.

    def transformer_layer(self, name, num_heads, num_kv_heads, head_dim,
                           intermediate):
        """One full decoder layer: self-attention + gated MLP. RMSNorm/
        LayerNorm has no MACs worth modeling and is omitted, matching
        how the CNN side already ignores pooling."""
        self.attention(f"{name}.attn", num_heads, num_kv_heads, head_dim)
        self.mlp_gated(f"{name}.mlp", intermediate)

    def bottleneck_mbv2(self, name, t, c_out, s):
        """MobileNetV2 inverted-residual block: expand(1x1) ->
        depthwise(3x3, stride s) -> project(1x1, linear)."""
        c_in = self.c
        c_exp = c_in * t
        if t != 1:
            self.conv(f"{name}.expand", c_exp, 1, 1)
        self.conv(f"{name}.dw", c_exp, 3, s, depthwise=True)
        self.conv(f"{name}.project", c_out, 1, 1)

    def bottleneck_c2f_inner(self, name, c_hidden):
        """One YOLOv8 Bottleneck *inside* a C2f block: two 3x3 dense
        convs, channel-preserving (cv1: c->c, cv2: c->c)."""
        self.conv(f"{name}.cv1", c_hidden, 3, 1)
        self.conv(f"{name}.cv2", c_hidden, 3, 1)

    def c2f(self, name, c_out, n):
        """YOLOv8 C2f block: cv1 (1x1, c_in -> 2*c_hidden) splits into
        two c_hidden branches; n inner Bottlenecks are chained on one
        branch, all n+2 intermediate tensors are concatenated
        (c_hidden*(n+2) channels), then cv2 (1x1) fuses down to c_out.
        Spatial size (h, w) is unaffected (stride 1 throughout)."""
        c_in = self.c
        c_hidden = c_out // 2
        self.conv(f"{name}.cv1", 2 * c_hidden, 1, 1)
        h, w = self.h, self.w  # unchanged by the 1x1 cv1 above
        # cv1's output conceptually splits into y0,y1 (c_hidden each); we
        # only need to track channel count for the chain below.
        self.c = c_hidden
        for i in range(n):
            self.bottleneck_c2f_inner(f"{name}.b{i}", c_hidden)
        # n+2 branches of c_hidden channels get concatenated.
        self.h, self.w, self.c = h, w, c_hidden * (n + 2)
        self.conv(f"{name}.cv2", c_out, 1, 1)

    def sppf(self, name, c_out):
        """YOLOv8 SPPF: cv1 (1x1, c_in -> c_in//2), three sequential 5x5
        maxpools (NOT modeled on the NPU - no MACs), concat of
        [x, pool1, pool2, pool3] (4 * c_in//2 channels), then cv2 (1x1)
        fuses down to c_out. Spatial size unaffected (pooling here uses
        stride 1 + padding, per the real SPPF module)."""
        c_in = self.c
        c_mid = c_in // 2
        self.conv(f"{name}.cv1", c_mid, 1, 1)
        h, w = self.h, self.w
        self.h, self.w, self.c = h, w, c_mid * 4  # after concat
        self.conv(f"{name}.cv2", c_out, 1, 1)


def expand_model(model):
    inp = model["input"]
    exp = Expander(inp["h"], inp["w"], inp["c"])

    for layer in model["layers"]:
        op = layer["op"]
        name = layer.get("name", op)
        if op == "conv":
            exp.conv(name, layer["out_c"], layer["k"], layer.get("s", 1))
        elif op == "bottleneck_mbv2":
            for i in range(layer["n"]):
                s = layer["s"] if i == 0 else 1
                exp.bottleneck_mbv2(f"{name}.{i}", layer["t"], layer["c"], s)
        elif op == "c2f":
            exp.c2f(name, layer["out_c"], layer["n"])
        elif op == "sppf":
            exp.sppf(name, layer["out_c"])
        elif op == "fc":
            exp.fc(name, layer["out_c"])
        elif op == "linear":
            exp.linear(name, layer["out_c"])
        elif op == "transformer_layer":
            num_kv_heads = layer.get("num_kv_heads", layer["num_heads"])
            for i in range(layer.get("n", 1)):
                exp.transformer_layer(
                    f"{name}.{i}", layer["num_heads"], num_kv_heads,
                    layer["head_dim"], layer["intermediate"]
                )
        else:
            raise ValueError(f"Unknown op '{op}' in layer '{name}'")

    return exp.ops


def prim_op_to_calls(op: PrimOp, max_m_per_call: int, max_n_per_call: int,
                      depthwise_channel_batch: int, bytes_per_elem: int):
    m_total = op.oh * op.ow
    calls = []

    if op.kind == "dense":
        k = op.kh * op.kw * op.cin
        n_total = op.cout
        # Tile both M and N: real NPU compilers tile the output matrix
        # in both dimensions when either exceeds the array/PARAM-field
        # limits - this matters a lot for e.g. an LM head projection
        # (N=vocab_size, often >> 65535), where M-only tiling wouldn't
        # be enough.
        for mi, m0 in enumerate(range(0, m_total, max_m_per_call)):
            m = min(max_m_per_call, m_total - m0)
            for ni, n0 in enumerate(range(0, n_total, max_n_per_call)):
                n = min(max_n_per_call, n_total - n0)
                _check_fields(op.name, m, k, n)
                len_bytes = m * bytes_per_elem  # activation tile (approx)
                calls.append(
                    NpuCall(f"{op.name}.tile{mi}_{ni}", m, k, n,
                             LAYER_TYPE_DENSE, len_bytes)
                )
    else:  # depthwise
        k = op.kh * op.kw
        # channel_batch=1 is the fully realistic "Cin separate calls"
        # model discussed in npu_accel.hh; a channel_batch > 1 is a
        # faster-but-less-realistic approximation for large models
        # where simulating every single channel is too slow, treating
        # depthwise_channel_batch channels as if issued together (still
        # correctly shows poor N-utilization, just fewer discrete FSM
        # runs in gem5 - use with care, it under-counts per-channel
        # control overhead by construction).
        n = min(depthwise_channel_batch, op.cin)
        remaining_channels = op.cin
        ch_idx = 0
        while remaining_channels > 0:
            this_n = min(n, remaining_channels)
            for mi, m0 in enumerate(range(0, m_total, max_m_per_call)):
                m = min(max_m_per_call, m_total - m0)
                _check_fields(op.name, m, k, this_n)
                len_bytes = m * bytes_per_elem
                calls.append(
                    NpuCall(f"{op.name}.ch{ch_idx}.tile{mi}", m, k, this_n,
                             LAYER_TYPE_DEPTHWISE, len_bytes)
                )
            remaining_channels -= this_n
            ch_idx += 1

    return calls


def _check_fields(name, m, k, n):
    for label, v in (("M", m), ("K", k), ("N", n)):
        if v <= 0 or v > 0xFFFF:
            raise ValueError(
                f"{name}: {label}={v} does not fit in the 16-bit PARAM0 "
                f"field (1..65535). Lower --max-m-per-call, or split this "
                f"layer's K/N further (not automated by this script)."
            )


def pack_param0(m, k, n):
    return (m << 32) | (k << 16) | n


def generate_header(model_name, prim_ops, all_calls, out_path):
    lines = []
    lines.append(f"/* Auto-generated by model_to_npu_calls.py from "
                  f"model '{model_name}'. Do not edit by hand. */")
    lines.append(f"#ifndef {model_name.upper()}_CALLS_H")
    lines.append(f"#define {model_name.upper()}_CALLS_H")
    lines.append("")
    lines.append('#include "common/gem5_accel.h"')
    lines.append("")
    lines.append("struct npu_call { uint64_t param0; uint64_t param1; "
                  "uint32_t len_bytes; const char *label; };")
    lines.append("")
    lines.append(f"static const struct npu_call "
                  f"{model_name}_calls[] = {{")
    for c in all_calls:
        param0 = pack_param0(c.m, c.k, c.n)
        lines.append(
            f'    {{ {param0}ULL, {c.layer_type}ULL, {c.len_bytes}u, '
            f'"{c.label}" }}, '
            f'/* M={c.m} K={c.k} N={c.n} */'
        )
    lines.append("};")
    lines.append(
        f"static const int {model_name}_num_calls = "
        f"{len(all_calls)};"
    )
    lines.append("")
    lines.append("#endif")
    out_path.write_text("\n".join(lines) + "\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model_json")
    ap.add_argument("--out", required=True, help="Output C header path")
    ap.add_argument("--max-m-per-call", type=int, default=4096,
                     help="Max output pixels/tokens (M) per NpuAccel call "
                          "(tiling limit; must be <= 65535)")
    ap.add_argument("--max-n-per-call", type=int, default=16384,
                     help="Max output channels/vocab entries (N) per "
                          "NpuAccel call (tiling limit; must be <= 65535). "
                          "Matters most for large N layers like an LM head "
                          "projecting to a large vocab_size.")
    ap.add_argument("--depthwise-channel-batch", type=int, default=1,
                     help="Channels grouped per depthwise call. 1 = fully "
                          "realistic (default, see file docstring); "
                          ">1 trades simulation realism for speed.")
    ap.add_argument("--bytes-per-elem", type=int, default=1,
                     help="Bytes per activation element (1=int8, "
                          "2=bf16/fp16, 4=fp32) - only affects the "
                          "len_bytes DMA size passed to gem5, not the "
                          "cycle-count model.")
    args = ap.parse_args()

    model = json.loads(Path(args.model_json).read_text())
    model_name = model.get("name", Path(args.model_json).stem)

    prim_ops = expand_model(model)

    all_calls = []
    total_macs = 0
    print(f"{'layer':40s} {'out HxWxC':16s} {'MACs':>14s} {'#calls':>8s}")
    print("-" * 84)
    for op in prim_ops:
        calls = prim_op_to_calls(op, args.max_m_per_call,
                                  args.max_n_per_call,
                                  args.depthwise_channel_batch,
                                  args.bytes_per_elem)
        all_calls.extend(calls)
        if op.kind == "dense":
            macs = op.oh * op.ow * op.kh * op.kw * op.cin * op.cout
        else:
            macs = op.oh * op.ow * op.kh * op.kw * op.cin  # N=1 per channel
        total_macs += macs
        shape = f"{op.oh}x{op.ow}x{op.cout}"
        kind_tag = "DW" if op.kind == "depthwise" else "  "
        print(f"[{kind_tag}] {op.name:36s} {shape:16s} {macs:14,d} "
              f"{len(calls):8d}")

    print("-" * 84)
    print(f"Total: {len(prim_ops)} primitive layers, {len(all_calls)} "
          f"NpuAccel calls, {total_macs:,d} theoretical MACs\n")

    out_path = Path(args.out)
    generate_header(model_name, prim_ops, all_calls, out_path)
    print(f"Wrote {out_path} ({len(all_calls)} calls)")


if __name__ == "__main__":
    sys.exit(main())
