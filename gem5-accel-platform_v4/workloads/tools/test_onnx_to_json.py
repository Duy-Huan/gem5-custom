#!/usr/bin/env python3
"""
test_onnx_to_json.py - builds REAL ONNX graphs (via onnx.helper, no
external downloads needed) with hand-computed, known-correct expected
MAC counts, runs them through onnx_to_json.py + model_to_npu_calls.py,
and asserts the results match exactly. Also does a round-trip test
against workloads/models/mobilenetv2.json's already-published-paper-
verified MAC total (300,774,272), by reconstructing an equivalent ONNX
graph and confirming the auto-converted JSON reproduces the same number.

Run: python3 test_onnx_to_json.py
Exits non-zero (and prints which assertion failed) on any mismatch.
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
import onnx
from onnx import helper, numpy_helper, TensorProto

HERE = Path(__file__).parent
ONNX_TO_JSON = HERE / "onnx_to_json.py"
MODEL_TO_CALLS = HERE / "model_to_npu_calls.py"


def make_conv_weight(name, out_c, in_c_per_group, k):
    arr = np.random.randn(out_c, in_c_per_group, k, k).astype(np.float32)
    return numpy_helper.from_array(arr, name=name)


def make_gemm_weight(name, out_features, in_features):
    arr = np.random.randn(out_features, in_features).astype(np.float32)
    return numpy_helper.from_array(arr, name=name)


def run_onnx_to_json(onnx_path, out_json_path, extra_args=None):
    cmd = [sys.executable, str(ONNX_TO_JSON), str(onnx_path),
           "--out", str(out_json_path)]
    if extra_args:
        cmd += extra_args
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError(f"onnx_to_json.py failed on {onnx_path}")
    return result.stdout


def run_model_to_calls(json_path, out_h_path, extra_args=None):
    cmd = [sys.executable, str(MODEL_TO_CALLS), str(json_path),
           "--out", str(out_h_path)]
    if extra_args:
        cmd += extra_args
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(result.stdout)
        print(result.stderr)
        raise RuntimeError(f"model_to_npu_calls.py failed on {json_path}")
    return result.stdout


import re


def total_macs_from_stdout(stdout):
    for line in stdout.splitlines():
        if line.startswith("Total:"):
            m = re.search(r"([\d,]+)\s+theoretical MACs", line)
            if m:
                return int(m.group(1).replace(",", ""))
    raise ValueError(f"could not find 'Total:' line in:\n{stdout}")


# ---------------------------------------------------------------------
# Test 1: simple sequential CNN, hand-computed exact expected MACs
# ---------------------------------------------------------------------
def test_simple_sequential(tmpdir):
    print("\n=== Test 1: simple sequential CNN (dense + depthwise + linear) ===")

    # Input: 1x3x16x16
    N, Cin, H, W = 1, 3, 16, 16

    # conv1: regular 3x3 stride 1 pad 1 (same), 3 -> 8 channels
    conv1_w = make_conv_weight("conv1.weight", 8, 3, 3)
    conv1 = helper.make_node(
        "Conv", ["input", "conv1.weight"], ["conv1_out"], name="conv1",
        kernel_shape=[3, 3], strides=[1, 1], pads=[1, 1, 1, 1], group=1,
    )
    # conv1 output: 8 x 16 x 16 (same padding)

    relu1 = helper.make_node("Relu", ["conv1_out"], ["relu1_out"], name="relu1")

    # dwconv: depthwise 3x3 stride 2 pad 1, 8 -> 8 channels (groups=8)
    dw_w = make_conv_weight("dw.weight", 8, 1, 3)  # (out_c, in_c/groups, k, k)
    dwconv = helper.make_node(
        "Conv", ["relu1_out", "dw.weight"], ["dw_out"], name="dwconv",
        kernel_shape=[3, 3], strides=[2, 2], pads=[1, 1, 1, 1], group=8,
    )
    # output: 8 x 8 x 8 (16 -> 8 via stride 2, same padding)

    # pointwise conv: 1x1, 8 -> 16 channels
    pw_w = make_conv_weight("pw.weight", 16, 8, 1)
    pwconv = helper.make_node(
        "Conv", ["dw_out", "pw.weight"], ["pw_out"], name="pwconv",
        kernel_shape=[1, 1], strides=[1, 1], pads=[0, 0, 0, 0], group=1,
    )
    # output: 16 x 8 x 8

    gap = helper.make_node(
        "GlobalAveragePool", ["pw_out"], ["gap_out"], name="gap"
    )
    # output: 16 x 1 x 1

    flatten = helper.make_node(
        "Flatten", ["gap_out"], ["flat_out"], name="flatten", axis=1
    )
    # output: (1, 16)

    fc_w = make_gemm_weight("fc.weight", 10, 16)  # out_features=10, in=16
    fc_b = numpy_helper.from_array(
        np.zeros(10, dtype=np.float32), name="fc.bias")
    fc = helper.make_node(
        "Gemm", ["flat_out", "fc.weight", "fc.bias"], ["fc_out"],
        name="fc", transB=1,
    )

    graph = helper.make_graph(
        [conv1, relu1, dwconv, pwconv, gap, flatten, fc],
        "simple_test",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT,
                                        [N, Cin, H, W])],
        [helper.make_tensor_value_info("fc_out", TensorProto.FLOAT,
                                        [N, 10])],
        initializer=[conv1_w, dw_w, pw_w, fc_w, fc_b],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)

    onnx_path = tmpdir / "simple_test.onnx"
    onnx.save(model, str(onnx_path))

    json_path = tmpdir / "simple_test.json"
    stdout1 = run_onnx_to_json(onnx_path, json_path, ["--verbose"])
    print(stdout1)

    calls_path = tmpdir / "simple_test_calls.h"
    stdout2 = run_model_to_calls(json_path, calls_path)
    print(stdout2)
    got_macs = total_macs_from_stdout(stdout2)

    # --- Hand-computed expected MACs ---
    # conv1: dense, Oh=Ow=16, K=3*3*3=27, N=8 -> MACs = 16*16*27*8
    conv1_macs = 16 * 16 * 3 * 3 * 3 * 8
    # dwconv: depthwise, Oh=Ow=8, K=3*3=9 per channel, Cin=8 channels
    #         (each channel: M=8*8=64, K=9, N=1 -> MACs = 64*9 per channel)
    dw_macs = 8 * 8 * 3 * 3 * 8  # oh*ow*kh*kw*cin (N=1 folded in)
    # pwconv: dense, Oh=Ow=8, K=1*1*8=8, N=16
    pw_macs = 8 * 8 * 1 * 1 * 8 * 16
    # fc: dense, M=1, K=16, N=10
    fc_macs = 1 * 16 * 10

    expected_macs = conv1_macs + dw_macs + pw_macs + fc_macs
    print(f"expected_macs = {expected_macs}, got_macs = {got_macs}")
    assert got_macs == expected_macs, (
        f"MISMATCH: expected {expected_macs}, got {got_macs}"
    )
    print("Test 1 PASSED (exact match)")


# ---------------------------------------------------------------------
# Test 2: grouped conv (ResNeXt-style, 1 < groups < Cin)
# ---------------------------------------------------------------------
def test_grouped_conv(tmpdir):
    print("\n=== Test 2: grouped conv (groups=4, not depthwise) ===")

    N, Cin, H, W = 1, 16, 8, 8
    # grouped conv: 16 -> 32 channels, groups=4 (4 in/group, 8 out/group)
    gw = make_conv_weight("gconv.weight", 32, 4, 3)  # (32, 16/4=4, 3, 3)
    gconv = helper.make_node(
        "Conv", ["input", "gconv.weight"], ["gconv_out"], name="gconv",
        kernel_shape=[3, 3], strides=[1, 1], pads=[1, 1, 1, 1], group=4,
    )

    graph = helper.make_graph(
        [gconv], "grouped_test",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT,
                                        [N, Cin, H, W])],
        [helper.make_tensor_value_info("gconv_out", TensorProto.FLOAT,
                                        [N, 32, H, W])],
        initializer=[gw],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)

    onnx_path = tmpdir / "grouped_test.onnx"
    onnx.save(model, str(onnx_path))
    json_path = tmpdir / "grouped_test.json"
    run_onnx_to_json(onnx_path, json_path, ["--verbose"])

    calls_path = tmpdir / "grouped_test_calls.h"
    stdout = run_model_to_calls(json_path, calls_path)
    print(stdout)
    got_macs = total_macs_from_stdout(stdout)

    # groups=4: 4 independent dense convs, each M=8*8=64, K=4*3*3=36, N=8
    expected_macs = 4 * (8 * 8 * 4 * 3 * 3 * 8)
    print(f"expected_macs = {expected_macs}, got_macs = {got_macs}")
    assert got_macs == expected_macs, (
        f"MISMATCH: expected {expected_macs}, got {got_macs}"
    )
    print("Test 2 PASSED (exact match)")


# ---------------------------------------------------------------------
# Test 3: residual/skip connection (Add merging two branches)
# ---------------------------------------------------------------------
def test_residual_add(tmpdir):
    print("\n=== Test 3: residual connection (Add) ===")

    N, Cin, H, W = 1, 16, 8, 8
    # branch: two 3x3 convs, channel-preserving (16->16->16)
    w1 = make_conv_weight("c1.weight", 16, 16, 3)
    c1 = helper.make_node(
        "Conv", ["input", "c1.weight"], ["c1_out"], name="c1",
        kernel_shape=[3, 3], strides=[1, 1], pads=[1, 1, 1, 1], group=1,
    )
    w2 = make_conv_weight("c2.weight", 16, 16, 3)
    c2 = helper.make_node(
        "Conv", ["c1_out", "c2.weight"], ["c2_out"], name="c2",
        kernel_shape=[3, 3], strides=[1, 1], pads=[1, 1, 1, 1], group=1,
    )
    # residual add: c2_out + input (shortcut)
    add = helper.make_node("Add", ["c2_out", "input"], ["add_out"], name="add")

    # one more conv after the merge, to confirm shape tracking survived
    # the Add correctly (channels must still be 16 going in)
    w3 = make_conv_weight("c3.weight", 24, 16, 3)
    c3 = helper.make_node(
        "Conv", ["add_out", "c3.weight"], ["c3_out"], name="c3",
        kernel_shape=[3, 3], strides=[1, 1], pads=[1, 1, 1, 1], group=1,
    )

    graph = helper.make_graph(
        [c1, c2, add, c3], "residual_test",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT,
                                        [N, Cin, H, W])],
        [helper.make_tensor_value_info("c3_out", TensorProto.FLOAT,
                                        [N, 24, H, W])],
        initializer=[w1, w2, w3],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)

    onnx_path = tmpdir / "residual_test.onnx"
    onnx.save(model, str(onnx_path))
    json_path = tmpdir / "residual_test.json"
    stdout1 = run_onnx_to_json(onnx_path, json_path, ["--verbose"])
    print(stdout1)

    calls_path = tmpdir / "residual_test_calls.h"
    stdout2 = run_model_to_calls(json_path, calls_path)
    print(stdout2)
    got_macs = total_macs_from_stdout(stdout2)

    # c1: M=8*8=64, K=16*3*3=144, N=16
    c1_macs = 64 * 16 * 3 * 3 * 16
    # c2: same shape as c1
    c2_macs = 64 * 16 * 3 * 3 * 16
    # c3: after Add (channels still 16, same spatial 8x8): K=16*3*3=144, N=24
    c3_macs = 64 * 16 * 3 * 3 * 24
    expected_macs = c1_macs + c2_macs + c3_macs

    print(f"expected_macs = {expected_macs}, got_macs = {got_macs}")
    assert got_macs == expected_macs, (
        f"MISMATCH: expected {expected_macs}, got {got_macs} "
        f"(this would indicate the Add shape-resync logic broke "
        f"channel tracking across the residual merge)"
    )
    print("Test 3 PASSED (exact match) - shape correctly resynced across Add")


# ---------------------------------------------------------------------
# Test 4: round-trip against the already paper-verified mobilenetv2.json
# ---------------------------------------------------------------------
def build_mbv2_bottleneck(nodes, inits, name, in_tensor, in_c, t, c_out, s, h, w):
    """Builds one MobileNetV2 inverted-residual block as real ONNX nodes,
    mirroring Expander.bottleneck_mbv2() in model_to_npu_calls.py exactly,
    so a round trip through onnx_to_json.py should reproduce the same
    MAC total as the hand-authored workloads/models/mobilenetv2.json."""
    c_exp = in_c * t
    cur = in_tensor
    cur_c, cur_h, cur_w = in_c, h, w

    if t != 1:
        w_exp = make_conv_weight(f"{name}.expand.weight", c_exp, cur_c, 1)
        inits.append(w_exp)
        nodes.append(helper.make_node(
            "Conv", [cur, f"{name}.expand.weight"], [f"{name}.expand_out"],
            name=f"{name}.expand", kernel_shape=[1, 1], strides=[1, 1],
            pads=[0, 0, 0, 0], group=1,
        ))
        cur = f"{name}.expand_out"
        cur_c = c_exp

    w_dw = make_conv_weight(f"{name}.dw.weight", cur_c, 1, 3)
    inits.append(w_dw)
    nodes.append(helper.make_node(
        "Conv", [cur, f"{name}.dw.weight"], [f"{name}.dw_out"],
        name=f"{name}.dw", kernel_shape=[3, 3], strides=[s, s],
        pads=[1, 1, 1, 1], group=cur_c,
    ))
    cur = f"{name}.dw_out"
    cur_h = (cur_h + 2 - 3) // s + 1
    cur_w = (cur_w + 2 - 3) // s + 1

    w_proj = make_conv_weight(f"{name}.project.weight", c_out, cur_c, 1)
    inits.append(w_proj)
    nodes.append(helper.make_node(
        "Conv", [cur, f"{name}.project.weight"], [f"{name}.project_out"],
        name=f"{name}.project", kernel_shape=[1, 1], strides=[1, 1],
        pads=[0, 0, 0, 0], group=1,
    ))
    return f"{name}.project_out", c_out, cur_h, cur_w


def test_mobilenetv2_roundtrip(tmpdir):
    print("\n=== Test 4: MobileNetV2 round-trip against paper-verified JSON ===")

    nodes, inits = [], []
    h, w, c = 224, 224, 3

    w1 = make_conv_weight("conv1.weight", 32, 3, 3)
    inits.append(w1)
    nodes.append(helper.make_node(
        "Conv", ["input", "conv1.weight"], ["conv1_out"], name="conv1",
        kernel_shape=[3, 3], strides=[2, 2], pads=[1, 1, 1, 1], group=1,
    ))
    cur, c, h, w = "conv1_out", 32, (h + 2 - 3) // 2 + 1, (w + 2 - 3) // 2 + 1

    # Table 2 of the MobileNetV2 paper (same config as
    # workloads/models/mobilenetv2.json):
    table2 = [
        ("bneck1", 1, 16, 1, 1),
        ("bneck2", 6, 24, 2, 2),
        ("bneck3", 6, 32, 3, 2),
        ("bneck4", 6, 64, 4, 2),
        ("bneck5", 6, 96, 3, 1),
        ("bneck6", 6, 160, 3, 2),
        ("bneck7", 6, 320, 1, 1),
    ]
    for name, t, c_out, n, s in table2:
        for i in range(n):
            stride = s if i == 0 else 1
            cur, c, h, w = build_mbv2_bottleneck(
                nodes, inits, f"{name}.{i}", cur, c, t, c_out, stride, h, w
            )

    w_last = make_conv_weight("conv_last.weight", 1280, c, 1)
    inits.append(w_last)
    nodes.append(helper.make_node(
        "Conv", [cur, "conv_last.weight"], ["conv_last_out"],
        name="conv_last", kernel_shape=[1, 1], strides=[1, 1],
        pads=[0, 0, 0, 0], group=1,
    ))
    cur, c = "conv_last_out", 1280

    nodes.append(helper.make_node(
        "GlobalAveragePool", [cur], ["gap_out"], name="gap"
    ))
    nodes.append(helper.make_node(
        "Flatten", ["gap_out"], ["flat_out"], name="flatten", axis=1
    ))
    fc_w = make_gemm_weight("classifier.weight", 1000, 1280)
    fc_b = numpy_helper.from_array(
        np.zeros(1000, dtype=np.float32), name="classifier.bias")
    inits += [fc_w, fc_b]
    nodes.append(helper.make_node(
        "Gemm", ["flat_out", "classifier.weight", "classifier.bias"],
        ["output"], name="classifier", transB=1,
    ))

    graph = helper.make_graph(
        nodes, "mobilenetv2_roundtrip",
        [helper.make_tensor_value_info("input", TensorProto.FLOAT,
                                        [1, 3, 224, 224])],
        [helper.make_tensor_value_info("output", TensorProto.FLOAT,
                                        [1, 1000])],
        initializer=inits,
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)

    onnx_path = tmpdir / "mobilenetv2_roundtrip.onnx"
    onnx.save(model, str(onnx_path))
    json_path = tmpdir / "mobilenetv2_roundtrip.json"
    run_onnx_to_json(onnx_path, json_path)

    calls_path = tmpdir / "mobilenetv2_roundtrip_calls.h"
    stdout = run_model_to_calls(json_path, calls_path)
    print(stdout)
    got_macs = total_macs_from_stdout(stdout)

    # This is the number already independently verified in this project
    # against the MobileNetV2 paper's published ~300M MAC figure (see
    # docs/ARCHITECTURE.md, "Đối chiếu để tự tin số liệu đúng").
    expected_macs = 300_774_272
    print(f"expected_macs (from hand-authored mobilenetv2.json) = "
          f"{expected_macs}, got_macs (from ONNX round-trip) = {got_macs}")
    assert got_macs == expected_macs, (
        f"ROUND-TRIP MISMATCH: hand-authored JSON gives {expected_macs} "
        f"MACs (paper-verified), but converting an equivalent ONNX graph "
        f"through onnx_to_json.py gives {got_macs} - a real bug "
        f"somewhere in the ONNX conversion path."
    )
    print("Test 4 PASSED (exact match with paper-verified MobileNetV2 MACs)")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        tmpdir = Path(tmp)
        test_simple_sequential(tmpdir)
        test_grouped_conv(tmpdir)
        test_residual_add(tmpdir)
        test_mobilenetv2_roundtrip(tmpdir)
    print("\nALL TESTS PASSED")


if __name__ == "__main__":
    main()
