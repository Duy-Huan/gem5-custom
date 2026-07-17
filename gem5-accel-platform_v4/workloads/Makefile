# Cross-compiles the benchmark binaries for gem5 SE-mode RISC-V simulation.
#
# Requires a RISC-V bare-metal or Linux userspace cross toolchain, e.g. on
# Ubuntu:  sudo apt install gcc-riscv64-linux-gnu
# or build one from https://github.com/riscv-collab/riscv-gnu-toolchain
#
# Usage:
#   make                 # generates model call headers from models/*.json,
#                        # then builds all seven benchmark binaries
#   make PY=python3.11   # override the python interpreter if needed

CC       ?= riscv64-linux-gnu-gcc
PY       ?= python3
CFLAGS   ?= -O2 -static -march=rv64gc -mabi=lp64d -I.
CONVERT  = tools/model_to_npu_calls.py

BASIC_BINARIES = npu_matmul_bench lpu_stream_bench isp_pipeline_bench
CNN_BINARIES   = npu_mobilenetv2_bench npu_yolov8n_bench
LLM_BINARIES   = npu_gemma2b_bench npu_gemma7b_bench
BINARIES       = $(BASIC_BINARIES) $(CNN_BINARIES) $(LLM_BINARIES)

GENERATED_HEADERS = models/mobilenetv2_calls.h models/yolov8n_calls.h \
                    models/gemma_2b_calls.h models/gemma_7b_calls.h

all: $(BINARIES)

# --- model JSON -> NpuAccel call header generation --------------------------
# Regenerates automatically whenever the model JSON or the converter
# script itself changes. Gemma's LM head projects to a 256,000-entry
# vocab (N), which needs --max-n-per-call tiling in addition to the
# default M tiling - see tools/model_to_npu_calls.py's docstring.
models/mobilenetv2_calls.h: models/mobilenetv2.json $(CONVERT)
	$(PY) $(CONVERT) $< --out $@

models/yolov8n_calls.h: models/yolov8n.json $(CONVERT)
	$(PY) $(CONVERT) $< --out $@

models/gemma_2b_calls.h: models/gemma_2b.json $(CONVERT)
	$(PY) $(CONVERT) $< --out $@ --max-n-per-call 16384

models/gemma_7b_calls.h: models/gemma_7b.json $(CONVERT)
	$(PY) $(CONVERT) $< --out $@ --max-n-per-call 16384

npu_mobilenetv2_bench: npu_mobilenetv2_bench.c common/gem5_accel.h \
                       common/npu_cnn_driver.h models/mobilenetv2_calls.h
	$(CC) $(CFLAGS) -o $@ $<

npu_yolov8n_bench: npu_yolov8n_bench.c common/gem5_accel.h \
                   common/npu_cnn_driver.h models/yolov8n_calls.h
	$(CC) $(CFLAGS) -o $@ $<

npu_gemma2b_bench: npu_gemma2b_bench.c common/gem5_accel.h \
                   common/npu_cnn_driver.h models/gemma_2b_calls.h
	$(CC) $(CFLAGS) -o $@ $<

npu_gemma7b_bench: npu_gemma7b_bench.c common/gem5_accel.h \
                   common/npu_cnn_driver.h models/gemma_7b_calls.h
	$(CC) $(CFLAGS) -o $@ $<

# --- simple single-call benchmarks (unchanged) ------------------------------
$(BASIC_BINARIES): %: %.c common/gem5_accel.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(BINARIES) $(GENERATED_HEADERS)

.PHONY: all clean
