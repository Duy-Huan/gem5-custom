/*
 * npu_gemma7b_bench.c - runs one forward pass (prefill over
 * models/gemma_7b.json's configured seq_len) of Gemma 7B through the
 * NPU accelerator model: every decoder layer's attention (plain MHA,
 * unlike 2B's MQA) + gated MLP projections, plus the LM head.
 *
 * IMPORTANT: this models Gemma (Google's open-weight model family), not
 * Gemini - see models/gemma_7b.json's "_gemini_vs_gemma" note.
 *
 * Regenerate the header after editing models/gemma_7b.json:
 *   python3 tools/model_to_npu_calls.py models/gemma_7b.json \
 *       --out models/gemma_7b_calls.h
 *
 * Build: see workloads/Makefile (it runs the step above automatically).
 */
#include "models/gemma_7b_calls.h"
#include "common/npu_cnn_driver.h"

int
main(void)
{
    RUN_NPU_CALLS(gemma_7b_calls, gemma_7b_num_calls, "Gemma-7B");
    return 0;
}
