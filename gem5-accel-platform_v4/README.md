# gem5 Accelerator Platform (NPU / LPU / ISP) — starter package

Bộ SimObject mẫu để mở rộng gem5 gốc (`gem5/gem5`, nhánh `stable`) thành
nền tảng mô phỏng nội bộ cho công ty bạn, gồm 3 accelerator memory-mapped:
**NPU** (systolic-array GEMM), **LPU** (streaming/dataflow token
processor), **ISP** (pipeline xử lý ảnh). Toàn bộ được xây trên hạ tầng
`PioDevice`/`DmaDevice`/`ClockedObject` thật của gem5 — không phải mock —
nên băng thông bus, tranh chấp bộ nhớ và độ trễ compute đều được mô phỏng
ở mức cycle thật.

Đã chốt theo lựa chọn của bạn:
- **Giao tiếp**: memory-mapped register (MMR) + DMA qua port, giống
  gem5-SALAM/gem5-Aladdin.
- **Nền hệ thống**: Syscall-Emulation (SE) mode, ISA RISC-V.
- **Độ chi tiết**: timing/cycle-level (systolic array, streaming
  pipeline, ISP pipeline — xem `docs/ARCHITECTURE.md`).

## Cấu trúc package

```
src/accelerators/         SimObject C++ + Python (copy thẳng vào gem5/src/)
src/arch-riscv-patch/     Đoạn code + hướng dẫn patch syscall SE-mode
configs/                  Config script gem5 + script chạy benchmark
workloads/                Chương trình benchmark C + Makefile cross-compile
workloads/models/         Config CNN & LLM thật (MobileNetV2, YOLOv8n, Gemma 2B/7B)
workloads/tools/          Script chuyển layer CNN/Transformer -> lệnh NPU (im2col)
workloads/common/         Header dùng chung (syscall wrapper, driver CNN/LLM)
docs/ARCHITECTURE.md      Thiết kế chi tiết, FSM, model thời gian, CNN/LLM mapping
docs/INTEGRATION.md       Hướng dẫn tích hợp từng bước vào gem5 gốc
docs/RTL-INTEGRATION.md   Hướng dẫn tích hợp RTL thật (Verilator/SystemC) từ gốc rễ
```

## Mô phỏng CNN thật

Ngoài 3 benchmark đơn giản (1 GEMM/1 batch/1 frame mỗi lần), package có
thêm `npu_mobilenetv2_bench` và `npu_yolov8n_bench` — chạy **toàn bộ**
layer của MobileNetV2 (chuẩn paper, input 224×224) và YOLOv8n backbone
(chuẩn Ultralytics, input 640×640) qua NPU model, layer theo layer, với
đúng kích thước/MAC thật của từng layer (kể cả xử lý đúng bản chất
depthwise conv — xem `docs/ARCHITECTURE.md` mục "Mô phỏng cả một mạng
CNN thật"). Muốn dùng model/kích thước khác: sửa file JSON trong
`workloads/models/` rồi `make` lại (script tự sinh lại header).

## Mô phỏng LLM thật — Gemma (không phải Gemini)

Có thêm `npu_gemma2b_bench` và `npu_gemma7b_bench` — chạy 1 lượt forward
(prefill) của **Gemma 2B/7B** (đúng `config.json` công bố chính thức của
Google) qua NPU: mọi phép chiếu Q/K/V/O, attention theo từng head, MLP
gated, và LM head. **Lưu ý**: dùng Gemma chứ không phải Gemini, vì Google
không công bố kiến trúc thật của Gemini — xem
`workloads/models/gemma_2b.json` mục `_gemini_vs_gemma` để biết chi tiết.
Đã tự đối chiếu MAC/token tính ra khớp gần tuyệt đối với số tham số
non-embedding tính tay từ chính config — xem `docs/ARCHITECTURE.md` mục
"Mô phỏng LLM thật".

Đã build & chạy cross-compile thật (`riscv64-linux-gnu-gcc`) cho **cả 7
benchmark** (3 đơn giản + 2 CNN + 2 LLM) trong quá trình phát triển gói
này để xác nhận không có lỗi cú pháp/link.

## Import model ONNX thật (mới)

`workloads/tools/onnx_to_json.py` chuyển **bất kỳ model CNN nào export ra
ONNX** (từ PyTorch, TensorFlow, hay framework nào khác) sang JSON schema
của nền tảng — không giới hạn ở 4 model mẫu viết tay ở trên. Khác với các
file JSON viết tay (dùng block cấp cao như `c2f`/`bottleneck_mbv2`),
script này đọc thẳng **shape inference thật của ONNX** ở từng node, nên
xử lý đúng cả model có nhánh rẽ/skip connection (residual), grouped conv
kiểu ResNeXt (không chỉ depthwise nhị phân), và pooling/flatten giữa các
lớp — không chỉ mô hình tuyến tính đơn giản.

**Đã kiểm chứng bằng 4 test tự động** (`workloads/tools/test_onnx_to_json.py`,
dựng ONNX thật bằng chính package `onnx`, không phải mock): 3 test so khớp
chính xác MAC tính tay, và 1 test round-trip dựng lại đúng kiến trúc
MobileNetV2 dưới dạng ONNX rồi convert — kết quả khớp **tuyệt đối
byte-for-byte** (300.774.272 MAC, 7277 lệnh) với số liệu đã đối chiếu paper
gốc ở mục "Mô phỏng CNN thật" bên trên. Chạy `python3
workloads/tools/test_onnx_to_json.py` để tự xác nhận lại bất cứ lúc nào
(cần `pip install onnx numpy`).

```bash
pip install onnx numpy
python3 workloads/tools/onnx_to_json.py your_model.onnx --out your_model.json
python3 workloads/tools/model_to_npu_calls.py your_model.json --out your_model_calls.h
```

## Bắt đầu nhanh

Xem `docs/INTEGRATION.md` để có các bước đầy đủ (clone gem5, copy file,
patch syscall, build, cross-compile, chạy). Tóm tắt:

```bash
git clone --branch stable https://github.com/gem5/gem5.git
cp -r src/accelerators <gem5-checkout>/src/accelerators
# ... áp patch syscall theo docs/INTEGRATION.md ...
cd <gem5-checkout> && scons build/RISCV/gem5.opt -j$(nproc)
cd workloads && make
build/RISCV/gem5.opt configs/accel_se_system.py --binary workloads/npu_matmul_bench
```

## Đã kiểm chứng với source thật

Toàn bộ API C++ dùng trong package này (ClockedObject, PioDevice, DmaDevice,
EventFunctionWrapper, statistics::Group/ADD_STAT, SyscallDescTable,
Process::pTable::translate...) được đối chiếu trực tiếp với source code
của gem5 nhánh `stable` (không suy đoán từ trí nhớ). Riêng phần **C
(userspace + cả 7 benchmark, bao gồm cả header 7277 lệnh tự sinh cho
MobileNetV2 và 1164 lệnh cho Gemma 7B)** đã **build & link thành công
thật** bằng `riscv64-linux-gnu-gcc` — không chỉ được viết mà chưa kiểm
chứng. Số liệu MAC của cả 4 model (MobileNetV2, YOLOv8n, Gemma 2B, Gemma
7B) đều đã được đối chiếu chéo với số liệu công bố/tính tay độc lập (chi
tiết trong `docs/ARCHITECTURE.md`). Điểm duy nhất **chưa** build-test
được trong môi trường này là một lần `scons build/RISCV/gem5.opt` đầy đủ
(phần C++ SimObject phía gem5), vì việc build toàn bộ gem5 mất khá nhiều
thời gian (thường 30–60+ phút) — bạn nên build và chạy thử
`debug-flags=Accel` đầu tiên khi tích hợp vào máy/CI của công ty để bắt
các lỗi cú pháp nhỏ (nếu có) do khác biệt phiên bản.

## Tích hợp RTL thật (mới)

Khi RTL team của bạn có sẵn thiết kế phần cứng thật (Verilog/SystemVerilog),
xem `docs/RTL-INTEGRATION.md` — hướng dẫn đầy đủ từ gốc rễ (Verilator hoạt
động ra sao, 2 kiến trúc tích hợp: Direct-in-SimObject vs SystemC/TLM bridge
có sẵn của gem5) kèm ví dụ **đã build & chạy thật** bằng Verilator 5.020:
một lõi MAC streaming (`src/accelerators/rtl/npu_mac_core.v`) được tích hợp
thành `NpuAccelRtl` — accelerator NPU thứ hai dùng RTL cosim thay vì công
thức phân tích, tồn tại song song với `NpuAccel` đã có. Tài liệu ghi rõ
từng phần đã kiểm chứng bằng cách nào (mục "Mức độ đã kiểm chứng") — không
có phần nào được trình bày như đã chạy thật nếu chưa thực sự chạy.

## Việc tiếp theo gợi ý

1. Build thử, chạy `npu_matmul_bench` với `--debug-flags=Accel` để xem
   FSM chạy đúng chưa.
2. Đối chiếu số cycle model đưa ra với datasheet/RTL thật của NPU/LPU/ISP
   công ty bạn, tinh chỉnh `computeLatency()`.
3. Nếu cần multi-op gối nhau hoặc buffer lớn hơn 1 trang, mở rộng theo
   mục "Giới hạn đã biết" trong `docs/ARCHITECTURE.md`.
4. Khi lên Full-System mode: bỏ qua syscall shim, gắn accelerator qua
   board/device-tree thật và viết driver kernel/UIO.
5. Khi có RTL thật: theo `docs/RTL-INTEGRATION.md`, bắt đầu từ bước verify
   RTL bằng testbench Verilator độc lập trước khi đụng vào gem5.
