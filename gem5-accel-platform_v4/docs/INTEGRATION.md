# Tích hợp vào gem5 repo gốc

Tài liệu này giả định bạn clone gem5 gốc (nhánh `stable`) từ
`https://github.com/gem5/gem5`, rồi áp bộ accelerator này lên trên. Toàn bộ
thiết kế đã được đối chiếu trực tiếp với source code hiện tại của nhánh
`stable` (không suy đoán API).

## Bước 1 — Clone gem5

```bash
git clone --branch stable https://github.com/gem5/gem5.git
cd gem5
git checkout -b company/accel-platform   # nhánh riêng cho công ty bạn
```

## Bước 2 — Copy các file SimObject

Copy toàn bộ `src/accelerators/` (từ package này) vào đúng vị trí:

```bash
cp -r /path/to/this/package/src/accelerators  <gem5-checkout>/src/accelerators
```

gem5's SConstruct tự động quét mọi `SConscript` dưới `src/`, nên bạn
**không cần sửa bất kỳ file build nào khác** cho phần NPU/LPU/ISP C++.

Cấu trúc sau khi copy:
```
src/accelerators/
  SConscript
  AccelBase.py       accel_base.hh      accel_base.cc
  NpuAccel.py        npu_accel.hh       npu_accel.cc
  LpuAccel.py        lpu_accel.hh       lpu_accel.cc
  IspAccel.py        isp_accel.hh       isp_accel.cc
```

## Bước 3 — Áp patch syscall cho chế độ SE (RISC-V)

File tham khảo: `src/arch-riscv-patch/gem5_accel_syscall.inc.cc` trong
package này. Đây **không phải** file được build trực tiếp — bạn cần dán nó
vào `src/arch/riscv/linux/se_workload.cc` thật trong checkout của mình.

1. Mở `<gem5-checkout>/src/arch/riscv/linux/se_workload.cc`.
2. Thêm các include cần thiết ngay sau các include hiện có ở đầu file:
   ```cpp
   #include <cerrno>
   #include "accelerators/accel_base.hh"
   #include "mem/page_table.hh"
   ```
3. Dán toàn bộ 3 hàm (`gem5AccelStartFunc`, `gem5AccelPollFunc`,
   `gem5AccelCyclesFunc` + helper `translateSingleBuffer`) từ
   `gem5_accel_syscall.inc.cc` vào **trước** định nghĩa
   `EmuLinux::syscallDescs64` trong cùng file.
4. Tìm dòng cuối cùng của bảng `syscallDescs64` — hiện tại kết thúc bằng
   entry `{2011, "getmainvars", ...}` — và thêm ngay sau đó (đừng quên
   dấu phẩy ở cuối entry `getmainvars` nếu thiếu):
   ```cpp
   {2012, "gem5_accel_start", gem5AccelStartFunc},
   {2013, "gem5_accel_poll", gem5AccelPollFunc},
   {2014, "gem5_accel_cycles", gem5AccelCyclesFunc},
   ```

> Lưu ý: đây là điểm tích hợp có khả năng cần điều chỉnh nhỏ nhất nếu
> phiên bản gem5 bạn dùng khác với bản `stable` mà package này được viết
> dựa trên — hãy `grep -n "getmainvars" se_workload.cc` để xác nhận đúng
> vị trí trước khi dán.

## Bước 4 — Build

```bash
scons build/RISCV/gem5.opt -j$(nproc)
```

Build RISCV-only (thay vì ALL) để rút ngắn thời gian build đáng kể — với
một target ISA, thời gian build lần đầu thường khoảng 30–60+ phút tùy máy.

Bật debug flag `Accel` khi cần theo dõi chi tiết FSM của accelerator:
```bash
build/RISCV/gem5.opt --debug-flags=Accel configs/accel_se_system.py \
    --binary workloads/npu_matmul_bench
```

## Bước 5 — Cross-compile benchmark

```bash
cd workloads
make CC=riscv64-linux-gnu-gcc
```

`make` tự chạy `tools/model_to_npu_calls.py` để sinh
`models/mobilenetv2_calls.h`, `models/yolov8n_calls.h`,
`models/gemma_2b_calls.h`, `models/gemma_7b_calls.h` từ file JSON trước
khi biên dịch — bạn sẽ thấy in ra bảng tóm tắt MAC/số lệnh từng layer.
Kết quả là 7 binary: 3 benchmark đơn giản (`npu_matmul_bench`,
`lpu_stream_bench`, `isp_pipeline_bench`), 2 benchmark CNN
(`npu_mobilenetv2_bench`, `npu_yolov8n_bench`), và 2 benchmark LLM
(`npu_gemma2b_bench`, `npu_gemma7b_bench` — mô phỏng Gemma, không phải
Gemini, xem `workloads/models/gemma_2b.json`). Toàn bộ 7 binary này đã
được build thử thành công thật với `riscv64-linux-gnu-gcc` trong quá
trình phát triển package — nếu build lỗi trên máy bạn, khả năng cao là do
thiếu gói `python3` (cần cho bước sinh header) hoặc phiên bản toolchain
khác.

## Bước 6 — Chạy

```bash
cd <gem5-checkout>
cp /path/to/package/configs/accel_se_system.py configs/
cp /path/to/package/configs/run_benchmark.py configs/
cp -r /path/to/package/workloads .

build/RISCV/gem5.opt configs/accel_se_system.py \
    --binary workloads/npu_matmul_bench

# chạy cả một mạng CNN thật (MobileNetV2 - sẽ mất nhiều thời gian mô
# phỏng hơn vì có tới ~7277 lệnh NPU, chủ yếu từ các layer depthwise):
build/RISCV/gem5.opt --debug-flags=Accel configs/accel_se_system.py \
    --binary workloads/npu_mobilenetv2_bench

# chạy 1 lượt forward của Gemma 2B (LLM thật, không phải Gemini):
build/RISCV/gem5.opt --debug-flags=Accel configs/accel_se_system.py \
    --binary workloads/npu_gemma2b_bench

# hoặc chạy cả 3 benchmark đơn giản và xem tóm tắt số liệu:
python3 configs/run_benchmark.py \
    --gem5 build/RISCV/gem5.opt \
    --config configs/accel_se_system.py
```

Kết quả chi tiết nằm trong `m5out/stats.txt` (hoặc thư mục
`m5out-accel/<tên-benchmark>/stats.txt` nếu dùng `run_benchmark.py`).
Với 2 benchmark CNN, ngoài `stats.txt`, `stdout` của chương trình RISC-V
(in qua `m5out/system.pc.com_1.device` hoặc console log tuỳ cấu hình)
cũng in ra bảng tóm tắt cycle/MAC theo từng layer — xem
`workloads/common/npu_cnn_driver.h` để biết chính xác định dạng in ra.

## Việc cần làm khi lên Full-System (FS) mode

Khi công ty bạn chuyển sang FS mode (boot Linux thật), **không cần đụng
tới `src/accelerators/`** — `AccelBase` đã dùng đúng `PioDevice`/`DmaDevice`
chuẩn của gem5 nên hoạt động y hệt qua MMIO thật. Bạn chỉ cần:
- Gắn 3 accelerator vào board FS (qua `Platform`/device tree hoặc ACPI
  tùy ISA) thay vì gắn thẳng vào `SystemXBar` như trong SE-mode script.
- Viết driver kernel thật (hoặc dùng UIO/`/dev/mem`) thay cho shim syscall
  ở Bước 3 — phần syscall chỉ dùng cho SE mode.
- Cân nhắc thêm interrupt (thay vì polling STATUS) — `AccelBase` hiện chưa
  có interrupt controller, đây là điểm mở rộng tự nhiên tiếp theo.
