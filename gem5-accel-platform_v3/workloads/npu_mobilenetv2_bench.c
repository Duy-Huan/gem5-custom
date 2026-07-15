/*
 * npu_mobilenetv2_bench.c - runs the full MobileNetV2 layer sequence
 * (as expanded + tiled by tools/model_to_npu_calls.py) through the NPU
 * accelerator model.
 *
 * Regenerate the header this includes after editing
 * models/mobilenetv2.json:
 *   python3 tools/model_to_npu_calls.py models/mobilenetv2.json \
 *       --out models/mobilenetv2_calls.h
 *
 * Build: see workloads/Makefile (it runs the step above automatically).
 */
#include "models/mobilenetv2_calls.h"
#include "common/npu_cnn_driver.h"

int
main(void)
{
    RUN_NPU_CALLS(mobilenetv2_calls, mobilenetv2_num_calls, "MobileNetV2");
    return 0;
}
