/*
 * npu_yolov8n_bench.c - runs the YOLOv8n backbone layer sequence (as
 * expanded + tiled by tools/model_to_npu_calls.py) through the NPU
 * accelerator model. See models/yolov8n.json for scope notes (backbone
 * only - no neck/head, see that file's "_scope" field for why).
 *
 * Regenerate the header this includes after editing models/yolov8n.json:
 *   python3 tools/model_to_npu_calls.py models/yolov8n.json \
 *       --out models/yolov8n_calls.h
 *
 * Build: see workloads/Makefile (it runs the step above automatically).
 */
#include "models/yolov8n_calls.h"
#include "common/npu_cnn_driver.h"

int
main(void)
{
    RUN_NPU_CALLS(yolov8n_backbone_calls, yolov8n_backbone_num_calls,
                  "YOLOv8n-backbone");
    return 0;
}
