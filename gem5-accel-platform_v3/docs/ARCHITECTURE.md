# Kiến trúc & quyết định thiết kế

## Sơ đồ tổng quan

```
                 ┌──────────────┐
                 │   CPU (RV64)  │
                 └──────┬────────┘
             icache/dcache│  (+ gem5_accel_* syscalls
                          │    trong SE mode)
                 ┌────────▼─────────┐
                 │   SystemXBar      │  <-- đo throughput/contention
                 │   (system.membus) │      của bus tại đây
                 └─┬──────┬──────┬──┘
        pio┌────────┘  pio│   pio│  ┌── dma (mỗi accel là 1 initiator
           │               │      │  │   độc lập trên cùng bus)
     ┌─────▼───┐    ┌──────▼──┐ ┌─▼──▼────┐
     │ NpuAccel │    │LpuAccel │ │IspAccel │
     └─────┬───┘    └──────┬──┘ └────┬────┘
           │  DMA (đọc/ghi thật qua RequestPort)
           └───────────────┴─────────┘
                          │
                 ┌────────▼────────┐
                 │  MemCtrl + DRAM   │  <-- đo bandwidth ở đây
                 └───────────────────┘
```

## Vì sao chọn MMR + DMA (thay vì custom instruction)

Bạn đã chọn phương án memory-mapped (MMR + DMA), giống pattern của
gem5-SALAM/gem5-Aladdin. Lý do đây là lựa chọn tốt cho nền tảng nội bộ:
- Không cần sửa ISA decoder/pipeline của CPU — accelerator là một
  `SimObject` độc lập, dễ bảo trì, dễ port sang ISA khác (x86/ARM) sau
  này nếu công ty cần.
- Giống hệt cách driver thật giao tiếp với hardware thật (ghi thanh ghi
  điều khiển, DMA dữ liệu) — kết quả mô phỏng có ý nghĩa tham chiếu tốt
  hơn khi so sánh với silicon thật.
- DMA đi qua port thật của gem5 → **traffic trên bus, tranh chấp
  (contention), và độ trễ bộ nhớ đều được mô phỏng thật**, không phải số
  liệu giả định — đúng với mục tiêu "đo throughput của bus" bạn nêu.

## FSM của mỗi accelerator

```
IDLE --(CTRL.START / syscall start)--> FETCH --(dmaRead xong)-->
COMPUTE --(hết N cycle theo model riêng)--> WRITEBACK
--(dmaWrite xong)--> DONE --(phần mềm ack STATUS=0)--> IDLE
```

- **FETCH/WRITEBACK**: dùng `DmaDevice::dmaRead()/dmaWrite()` có sẵn của
  gem5 → tự động xử lý retry, chia gói theo cache-line, và sinh Packet
  thật trên `RequestPort`.
- **COMPUTE**: dùng `EventFunctionWrapper` lên lịch sau
  `cyclesToTicks(N)` — N cycle tính theo model riêng của từng accelerator
  (xem `computeLatency()` trong mỗi `.cc`).

## Model thời gian tính toán (v2 — có tiling/padding + util_factor)

| Accelerator | Ý tưởng model | Công thức |
|---|---|---|
| NPU | Systolic array `pe_rows x pe_cols`, tile hoá theo output MxN, weight-stationary | `rawCycles = ceil(M/rows) * ceil(N/cols) * (weight_load_cycles + K + rows+cols-1)`<br>`cycles = ceil(rawCycles / util_factor)` |
| LPU | Dataflow streaming, throughput-bound theo token, có overhead nạp context mỗi lần gọi | `rawCycles = setup_cycles + seq_len*ceil(model_dim/lanes) + pipeline_depth`<br>`cycles = ceil(rawCycles / util_factor)` |
| ISP | Pipeline nhiều stage, streaming theo pixel, mỗi stage có chi phí đồng bộ riêng | `rawCycles = ceil(width*height/pixels_per_cycle) + num_stages*stage_overhead_cycles`<br>`cycles = ceil(rawCycles / util_factor)` |

**Hai thứ mới so với bản v1 (mục "Việc tiếp theo gợi ý" ở README trước
đó), theo đúng yêu cầu bổ sung hệ số hiệu chỉnh:**

1. **Padding/tiling waste — tính rõ thay vì lấy trung bình.**
   v1 tính `M*K*N / (rows*cols)` — coi như phần cứng luôn được dùng hết,
   kể cả khi `M`/`N` không chia hết cho `pe_rows`/`pe_cols`. Thực tế
   systolic array xử lý theo **tile** cố định kích thước `rows x cols`;
   tile cuối cùng ở mỗi chiều dù chỉ dùng một phần mảng vẫn tốn trọn thời
   gian một tile. v2 dùng `ceil(M/rows) * ceil(N/cols)` (đếm tile) thay vì
   chia tổng MAC — nên phần lãng phí này hiện ra tự nhiên trong số cycle,
   không cần thêm hệ số riêng. Tương tự, LPU có `setup_cycles` (chi phí cố
   định mỗi lần gọi, không phụ thuộc `seq_len`) mô phỏng đúng hiệu ứng
   "batch nhỏ trả overhead tương đối cao hơn"; ISP có `stage_overhead_cycles`
   thay cho `+1 cycle/stage` cố định trước đây — số stage giờ ảnh hưởng
   thật đến chi phí thay vì gần như không đáng kể.

2. **`util_factor` — hệ số hiệu chỉnh tổng thể.** Một số `(0, 1]` áp dụng
   sau cùng: `cycles = ceil(rawCycles / util_factor)`. Đây là "van xả" cho
   mọi thứ mô hình analytical **không** cố tình tính chi tiết — bong bóng
   pipeline ở control logic, double-buffering không hoàn hảo, mất mát khi
   FETCH/COMPUTE/WRITEBACK không chồng lấp (xem mục giới hạn #4 bên dưới).
   `util_factor = 1.0` nghĩa là "tin tưởng tuyệt đối công thức toán học".
   Khi có số đo thật từ RTL/silicon, chỉnh `util_factor` sao cho
   `cycles` mô hình khớp với cycle đo được — đây là bước hiệu chỉnh
   (calibration) thông thường của mọi mô hình hiệu năng analytical.

Ví dụ tác động cụ thể: GEMM `M=17, K=64, N=17` trên mảng `16x16`
- v1: `ceil(17*64*17/(16*16)) + 31 = 74 + 31 = 105` cycle — coi mảng dùng
  gần hết dù M,N chỉ nhỉnh hơn 16 một chút.
- v2 (util_factor=1.0): `ceil(17/16)*ceil(17/16) = 2*2 = 4` tile, mỗi tile
  `64+31+64(weight_load mặc định) = 159` cycle → `4*159 = 636` cycle — vì
  thực tế cần tới 4 tile (2x2) dù 3 trong số đó chỉ dùng ~1 hàng/cột hữu
  ích của mảng. Đây chính là cái giá thật của việc chọn kích thước bài
  toán không "vừa khít" phần cứng — thứ v1 hoàn toàn bỏ sót.

Đây vẫn là model bậc-một/bậc-hai (chưa phải cycle-accurate tuyệt đối),
đúng dạng thường dùng để **sizing** kiến trúc (chọn số PE, số lane, số
stage) và để so sánh tương đối giữa các cấu hình. Khi cần độ chính xác
cao hơn nữa (bank conflict trong scratchpad, double buffering thật,
sparsity, stall vì tranh chấp DMA...) đây chính là các hàm
`computeLatency()` cần mở rộng tiếp — kiến trúc FSM/DMA xung quanh không
cần đổi.

## Mô phỏng cả một mạng CNN thật (v3 — YOLOv8n / MobileNetV2)

Thay vì gọi NPU với 1 bộ (M,K,N) tự chọn, bạn có thể mô phỏng **toàn bộ
một mạng CNN thật**, layer theo layer, với đúng kích thước/MAC của từng
layer. Luồng xử lý:

```
models/mobilenetv2.json  ──┐
(hoặc yolov8n.json)        │  tools/model_to_npu_calls.py
                            ▼
                  giải nén block cấp cao (bottleneck/c2f/sppf)
                  thành từng conv2d/depthwise/fc nguyên thuỷ,
                  tính (M,K,N,layer_type) qua im2col,
                  chia tile nếu M vượt --max-m-per-call
                            │
                            ▼
              models/<model>_calls.h  (mảng {param0,param1,len_bytes,label})
                            │
                            ▼
        npu_mobilenetv2_bench.c / npu_yolov8n_bench.c
        (dùng chung common/npu_cnn_driver.h để loop qua từng lệnh,
         gọi gem5_accel_start/wait/cycles, cộng dồn thống kê)
```

### Ánh xạ layer CNN thật sang (M, K, N) — phép im2col chuẩn

- **conv2d / pointwise (1x1) / fully-connected** (GEMM dày đặc thật sự):
  `M = Oh*Ow` (số vị trí output), `K = Kh*Kw*Cin` (kích thước patch x số
  kênh vào), `N = Cout`. Đây là công thức im2col chuẩn mọi compiler NPU
  dùng.
- **depthwise conv** (lõi của MobileNet, `groups == Cin`): **không phải**
  một GEMM dày đặc duy nhất — output kênh `c` chỉ phụ thuộc input kênh
  `c`, không có phép reduction chung qua các kênh như conv thường. Cách
  chạy đúng bản chất trên một systolic array dày đặc là tách thành
  **Cin lệnh nhỏ riêng biệt**: mỗi lệnh `M=Oh*Ow, K=Kh*Kw, N=1`. Với
  `N=1`, một tile chỉ dùng đúng 1 trong `pe_cols` cột — công thức tile
  cũ (đã có ở v2) tự động phản ánh đúng sự lãng phí này, **không cần sửa
  gì thêm trong công thức cycle**.
- `PARAM1` giờ mã hoá `layer_type` (bit0: 0=DENSE, 1=DEPTHWISE) — dùng để
  DPRINTF/thống kê phân loại rõ dense vs depthwise, và để cộng thêm
  `depthwise_call_overhead_cycles` (chi phí control-plane/descriptor mỗi
  lệnh, chỉ tính cho depthwise vì đây là lệnh phát sinh lặp lại Cin lần
  cho 1 layer logic — chi phí này ở conv thường không đáng kể so với
  khối lượng tính trong 1 lệnh, nhưng ở depthwise (bị gọi rất nhiều lần
  cho cùng 1 layer) thì cộng dồn đáng kể).

### Vì sao việc này quan trọng — ví dụ thật đã chạy thử

Chạy `model_to_npu_calls.py` trên 2 model thật (input 224×224 cho
MobileNetV2 theo đúng paper gốc, 640×640 cho YOLOv8n theo đúng cấu hình
Ultralytics — số liệu bên dưới đã chạy thật, không phải ước lượng):

| | MobileNetV2 | YOLOv8n (chỉ backbone) |
|---|---|---|
| Tổng MAC lý thuyết | 300,774,272 (~300M — khớp con số ~300M MAC / 0.3 GMACs công bố chuẩn của paper) | 6,222,643,200 (~6.2 GMAC — ~70% của tổng ~8.9 GFLOPs công bố cho toàn mạng đầy đủ backbone+neck+head, hợp lý vì backbone chiếm phần lớn compute) |
| Số layer nguyên thuỷ | 53 | 27 |
| Số lệnh NPU (đã tile) | **7,277** | 88 |
| Kiểu layer | Có depthwise (hầu hết) | Toàn conv thường (YOLOv8 không dùng depthwise) |

Điểm đáng chú ý nhất: MobileNetV2 có **MAC ít hơn YOLOv8n backbone tới
~20 lần** (300M so với 6.2G), nhưng lại cần **~83 lần nhiều lệnh NPU hơn**
(7277 so với 88) — vì gần như mọi block của MobileNetV2 đều có 1 lớp
depthwise, và mỗi lớp depthwise bị tách thành hàng trăm lệnh nhỏ (bằng số
kênh input). Đây chính xác là hiện tượng thật trong công nghiệp: **mạng
"nhẹ FLOPs" như MobileNet không nhất thiết chạy nhanh trên NPU dạng
systolic array dày đặc**, vì phần cứng đó được thiết kế cho GEMM lớn, không
phải hàng loạt phép nhân ma trận tí hon N=1. Model bậc-một của bạn giờ đã
thể hiện đúng insight kiến trúc này mà không cần thêm dòng code đặc biệt
nào trong công thức cycle — nó tự "rơi ra" từ việc mô hình tile đúng.

### Đối chiếu để tự tin số liệu đúng

Trước khi giao, mình đã kiểm tra chéo 2 điểm để tránh bịa số:
1. Tổng MAC MobileNetV2 tính ra (~300M) khớp con số chuẩn công bố trong
   paper gốc (Sandler et al. 2018).
2. Số tham số (params) tính tay cho block `C2f[64,64,n=1]` của YOLOv8n
   (cv1 + n×2 bottleneck-conv + cv2 = 4,096+9,216+9,216+6,144=28,672) khớp
   gần đúng con số thật từ một bản in model YOLOv8n thật (29,056 params,
   lệch ~1.3%, nhiều khả năng do làm tròn `c_hidden`) — xác nhận cấu trúc
   C2f mình giải nén đúng bản chất module thật của Ultralytics.
3. Toàn bộ 5 chương trình benchmark (kể cả `npu_mobilenetv2_bench` với
   header 7277 lệnh sinh tự động) đã **build & link thành công thật** bằng
   `riscv64-linux-gnu-gcc` trong quá trình phát triển gói này — không chỉ
   là code chưa kiểm chứng.

### Giới hạn đã biết của phần CNN (và hướng mở rộng)

1. **YOLOv8n mới chỉ có backbone** (9 layer đầu: stem+C2f+SPPF), **chưa
   có neck (FPN/PAN) và detect head** — vì neck cần concat nhiều feature
   map từ các layer trước đó (rẽ nhánh/skip connection), còn
   `Expander` hiện tại chỉ theo dõi **một** tensor "hiện tại" tuyến tính.
   Mở rộng: đổi `Expander` thành một DAG nhỏ (lưu tensor theo tên ở các
   điểm rẽ nhánh + thêm op `concat`) — cấu trúc `AccelBase`/`NpuAccel`
   phía dưới không cần đổi gì.
2. **Depthwise mô phỏng "đầy đủ nhất" (N=1, tách Cin lệnh riêng) khá tốn
   thời gian mô phỏng** (MobileNetV2 ra 7277 lệnh, tương ứng 7277 chu kỳ
   FETCH→COMPUTE→WRITEBACK DMA thật trong gem5). Script có cờ
   `--depthwise-channel-batch N` để gộp N kênh/lệnh — nhanh hơn nhưng kém
   thực tế hơn (không tính đúng chi phí control-plane từng kênh riêng).
3. **Padding "same-ish" (`pad = k//2`) là giả định**, không đọc từ config
   thật của từng layer (một số layer có thể dùng padding khác). Với
   YOLOv8/MobileNet chuẩn thì giả định này đúng cho gần hết các layer.
4. **Không tự động parse model thật từ PyTorch/ONNX** — JSON hiện tại là
   mình tra cứu/tính tay từ kiến trúc công bố. Muốn tự động hoá: viết một
   script Python dùng `torch.fx`/`onnx` duyệt qua từng node conv thật của
   model đã train, xuất ra đúng JSON schema này — không cần đổi gì ở
   `model_to_npu_calls.py` hay phía C++.

## Mô phỏng LLM thật — Gemma, không phải Gemini (v4)

**Lưu ý quan trọng trước tiên**: bạn hỏi thêm "model Gemini" cho NPU, nhưng
Google **không công bố** kiến trúc thật của Gemini (hidden size, số layer,
cấu hình attention...) — đây là model đóng. Cái công bố công khai, cùng
công nghệ/nghiên cứu với Gemini (theo chính lời Google), là **Gemma** —
package này dùng Gemma vì đó là thứ duy nhất có `config.json` thật để đối
chiếu, thay vì bịa số cho "Gemini". Nếu Google công bố thông số Gemini
thật, chỉ cần đổi số trong JSON — cấu trúc block (transformer_layer với
GQA/MQA) đã tổng quát hoá sẵn.

### Ánh xạ layer Transformer sang (M, K, N)

Không như CNN, trạng thái "hiện tại" của Transformer là `(seq_len,
hidden_dim)` thay vì `(H, W, C)` — field `input.h` trong JSON giờ đóng vai
trò `seq_len` (số token), `input.c` là `hidden_dim`. Mọi phép chiếu tuyến
tính (Q/K/V/O proj, gate/up/down proj MLP, LM head) đều là GEMM dày đặc
chuẩn: `M=seq_len, K=in_features, N=out_features` — về bản chất giống hệt
conv 1x1 đã dùng cho CNN, chỉ đổi tên biến.

**Phần đáng chú ý nhất — tự-attention không phải 1 GEMM lớn**: mỗi
attention head độc lập với các head khác (không chia sẻ chiều reduction),
nên `QK^T` và `softmax(scores)@V` được tách thành **`num_heads` lệnh GEMM
riêng biệt** (`M=seq_len, K=head_dim, N=seq_len` rồi `M=seq_len,
K=seq_len, N=head_dim`) — giống tinh thần "tách theo channel" của
depthwise conv, nhưng khác ở chỗ **mỗi lệnh vẫn là GEMM dày đặc đầy đủ**
(không mất mát N=1 như depthwise) — chỉ là `head_dim` (thường ~256) khá
nhỏ so với `seq_len`, nên các lệnh này K-nhẹ nhưng M/N vẫn lớn (không tệ
như depthwise, nhưng vẫn là chi phí thật đáng model riêng vì attention
scale theo `seq_len²` trong khi các phép chiếu tuyến tính chỉ scale theo
`seq_len`).

GQA/MQA được hỗ trợ tổng quát qua `num_kv_heads` (Gemma 2B dùng
`num_kv_heads=1` — multi-query attention thật, Gemma 7B dùng
`num_kv_heads=16=num_heads` — multi-head attention chuẩn, không GQA/MQA ở
size này — cả hai đều đúng theo `config.json` công bố, không phải mình tự
chọn).

### Đã kiểm chứng bằng một phép tự-đối-chiếu độc lập

Chạy thử `gemma_2b.json` (seq_len=128) cho tổng **321,988,329,472 MAC**;
trừ phần LM head (67,108,864,000 MAC) còn lại **254,879,465,472 MAC cho
128 token ≈ 1.99 tỷ MAC/token**. Tính tay số tham số non-embedding từ
chính `config.json` (4 phép chiếu attention + 3 phép chiếu MLP mỗi layer,
×18 layer) ra **≈1.98 tỷ tham số** — khớp gần như tuyệt đối với quy luật
kinh điển "MACs/token (1 lượt forward) ≈ số tham số non-embedding". Với
`gemma_7b.json` cũng ra kết quả tự nhất quán tương tự (~7.78 GMac/token
so với ~7.75 tỷ tham số non-embedding tính tay). Đây là bằng chứng độc
lập cho thấy phép ánh xạ GEMM/attention của mình đúng, không chỉ là code
chạy được mà không sai ở đâu. Toàn bộ 2 benchmark (`npu_gemma2b_bench`,
`npu_gemma7b_bench`) cũng đã build & link thành công thật bằng
`riscv64-linux-gnu-gcc`, giống các benchmark CNN.

### Một tính năng mới cần thiết cho LLM: chia tile theo N

LM head chiếu `hidden -> vocab_size` (256,000 cho Gemma) — vượt xa giới
hạn 16-bit của field `N` (65535). `model_to_npu_calls.py` giờ chia tile
theo **cả M lẫn N** (trước đây — mục CNN — chỉ chia M, vì CNN hiếm khi có
`N=Cout` lớn tới mức đó). Cờ mới: `--max-n-per-call` (mặc định 16384).

### Giới hạn riêng của phần LLM

1. **Chỉ mô hình 1 lượt forward (prefill)**, không mô hình decode
   autoregressive từng token một (với KV-cache) — đây là chế độ chạy
   khác hẳn về đặc tính (M=1 mỗi bước thay vì M=seq_len), cần một Expander
   riêng nếu bạn cần benchmark latency decode thay vì throughput prefill.
2. **RMSNorm, RoPE, softmax không có MAC** nên không gửi xuống NPU — nhất
   quán với việc bỏ qua maxpool/pooling ở phần CNN.
3. **`_source`/`_gemini_vs_gemma` trong JSON đã ghi rõ**: đây là Gemma
   (Gemma 1: 2B/7B), không phải Gemma 2/3 (kiến trúc có sliding-window
   attention xen kẽ, khác với Gemma 1) — chọn Gemma 1 vì đây là bản có số
   liệu được xác nhận chéo nhất quán nhất qua nhiều nguồn độc lập trong
   quá trình tra cứu.

## Giới hạn chung của accelerator (áp dụng cho mọi loại NPU/LPU/ISP)

1. **Một thao tác tại một thời điểm / accelerator** (không pipeline được
   nhiều lệnh gối nhau). Mở rộng: thêm hàng đợi lệnh (command queue) +
   nhiều `scratch` buffer.
2. **Buffer DMA trong SE mode phải nằm gọn trong 1 trang 4KiB** (do syscall
   shim chỉ dịch địa chỉ đầu). Mở rộng: scatter/gather nhiều trang trong
   `AccelBase`, hoặc gọi nhiều lần theo từng trang từ benchmark.
3. **Không có ngắt (interrupt)** — phần mềm phải polling `STATUS`. Ở FS
   mode nên bổ sung interrupt controller thật (xem INTEGRATION.md).
4. **DMA là "bounce buffer" tuyến tính** (`dmaRead` toàn bộ rồi mới
   compute rồi mới `dmaWrite`) — chưa chồng lấp fetch/compute/writeback
   của cùng một op. Với workload lớn, cách này sẽ đánh giá thấp
   throughput thực tế của một pipeline được thiết kế tốt; nếu cần mô
   hình sát hơn, tách `FETCH`/`COMPUTE` theo từng tile/block và cho chồng
   lấp (double buffering). Đây cũng là phần mà `util_factor` đang "gộp
   chung" tạm thời — nếu tách được FETCH/COMPUTE/WRITEBACK ra chồng lấp
   thật, bạn có thể bỏ bớt phần derate cho hiệu ứng này khỏi `util_factor`.
5. **`weight_load_cycles`/`setup_cycles`/`stage_overhead_cycles` là tham số
   phẳng (constant), chưa phụ thuộc kích thước dữ liệu thật** (ví dụ nạp
   weight nhanh/chậm tuỳ băng thông scratchpad thật). Đây là bước hiệu
   chỉnh tiếp theo hợp lý sau `util_factor`: đổi các hằng số này thành
   hàm của kích thước tile/lane thay vì số cố định.
6. **Không mô hình năng lượng.** Có thể tích hợp thêm McPAT/CACTI dựa trên
   `opsCompleted`/`busyCycles`/`bytesTransferred` đã có sẵn trong stats.
