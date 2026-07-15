/*
 * npu_gemma2b_bench.c - runs one forward pass (prefill over
 * models/gemma_2b.json's configured seq_len) of Gemma 2B through the
 * NPU accelerator model: every decoder layer's attention (MQA) + gated
 * MLP projections, plus the LM head.
 *
 * IMPORTANT: this models Gemma (Google's open-weight model family), not
 * Gemini - Google has not published Gemini's exact architecture. See
 * models/gemma_2b.json's "_gemini_vs_gemma" note for details.
 *
 * Regenerate the header after editing models/gemma_2b.json:
 *   python3 tools/model_to_npu_calls.py models/gemma_2b.json \
 *       --out models/gemma_2b_calls.h
 *
 * Build: see workloads/Makefile (it runs the step above automatically).
 */
#include "models/gemma_2b_calls.h"
#include "common/npu_cnn_driver.h"

int
main(void)
{
    RUN_NPU_CALLS(gemma_2b_calls, gemma_2b_num_calls, "Gemma-2B");
    return 0;
}
