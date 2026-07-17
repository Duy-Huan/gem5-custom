# Tích hợp RTL thật vào gem5 — hướng dẫn kỹ thuật từ gốc rễ

> Tài liệu tham chiếu nội bộ. Đi kèm ví dụ hoạt động thật (`rtl/npu_mac_core.v`
> + `rtl/test/standalone_test.cpp`) đã được biên dịch và chạy thành công bằng
> Verilator 5.020 trong quá trình soạn tài liệu này — xem mục 7 để biết chính
> xác phần nào đã kiểm chứng, phần nào là thiết kế theo pattern có sẵn nhưng
> chưa build-test end-to-end trong gem5 đầy đủ.

## Mục lục
1. [Vì sao cần RTL cosimulation, và nó khác gì với model phân tích](#1)
2. [Verilator hoạt động như thế nào — nền tảng kỹ thuật](#2)
3. [Hai kiến trúc tích hợp RTL vào gem5](#3)
4. [Ví dụ hoạt động: NPU MAC core thật](#4)
5. [Từng bước tích hợp (Direct-in-SimObject)](#5)
6. [Kiến trúc thay thế: SystemC/TLM bridge (hạ tầng có sẵn của gem5)](#6)
7. [Mức độ đã kiểm chứng của tài liệu này](#7)
8. [Cạm bẫy kỹ thuật thường gặp](#8)
9. [Phụ lục: toàn bộ code tham chiếu](#9)

---

## 1. Vì sao cần RTL cosimulation, và nó khác gì với model phân tích <a name="1"></a>

Trong `docs/ARCHITECTURE.md` của nền tảng này, NPU/LPU/ISP được mô hình bằng
**công thức phân tích** (analytical model): `computeLatency()` nhận (M,K,N)
hoặc (width,height) v.v., trả về một số chu kỳ ước lượng dựa trên công thức
toán học (tiling, pipeline fill/drain, hệ số hiệu chỉnh `util_factor`).

Model phân tích có giá trị lớn ở giai đoạn **sizing** (chọn số PE, số lane,
băng thông bus) vì chạy cực nhanh và không cần RTL. Nhưng nó có giới hạn nội
tại: **độ chính xác phụ thuộc vào việc công thức có đúng với phần cứng thật
hay không** — và với logic phức tạp (pipeline nhiều giai đoạn có forwarding,
stall động, arbitration giữa nhiều luồng, hiệu ứng bank conflict thật...),
không công thức bậc-một/bậc-hai nào nắm bắt hết.

**RTL cosimulation giải quyết đúng vấn đề này**: thay vì "đoán" số chu kỳ,
bạn *chạy thật* logic RTL (từng cổng, từng thanh ghi, đúng như khi ra
silicon) bên trong gem5, đếm số chu kỳ **thật sự** logic đó cần để hoàn
thành, rồi để gem5 dùng con số đó để tính thời điểm hoàn tất thao tác (giống
hệt cách `computeLatency()` được dùng, chỉ khác nguồn gốc con số: đo thật
thay vì tính công thức).

Đánh đổi: RTL cosim chạy chậm hơn nhiều (mô phỏng từng cạnh xung nhịp thay
vì tính 1 công thức), và cần RTL đã có sẵn (không dùng được ở giai đoạn
"chưa có gì cả" của model phân tích). Vì vậy hai cách **bổ sung cho nhau**
theo từng giai đoạn dự án, không thay thế nhau — nền tảng này giữ cả
`NpuAccel` (phân tích, đã có) và `NpuAccelRtl` (RTL, tài liệu này thêm vào)
cùng tồn tại song song, chọn cái nào dùng cái đó tùy giai đoạn/độ tin cậy
cần có.

---

## 2. Verilator hoạt động như thế nào — nền tảng kỹ thuật <a name="2"></a>

**Verilator** là một trình biên dịch Verilog/SystemVerilog mã nguồn mở,
nhưng khác với simulator RTL truyền thống (VCS, Questa, Icarus...) —
Verilator **không mô phỏng RTL trực tiếp**. Nó **dịch RTL sang mã C++**
(gọi là "Verilated model"), sau đó bạn biên dịch mã C++ đó và tự viết một
chương trình C++ (gọi là *testbench*/*driver*) để lái nó. Đây là lý do
Verilator nhanh hơn nhiều so với simulator sự kiện rời rạc truyền thống —
và cũng là lý do nó **ghép nối tự nhiên với gem5** (gem5 cũng là một
chương trình C++, nên "lái" một Verilated model từ bên trong một SimObject
gem5 chỉ là gọi hàm C++ bình thường).

Ba khái niệm cốt lõi cần hiểu trước khi đọc code:

- **`eval()`**: hàm do Verilator sinh ra, đại diện cho "để mạch tổ hợp lan
  truyền tín hiệu tại thời điểm hiện tại". Sau khi bạn đổi giá trị input
  (ví dụ `dut->in_valid = 1`), bạn **phải** gọi `eval()` để mạch "tính lại"
  — giống hệt việc chờ tín hiệu ổn định trên mạch thật.
- **Cạnh xung nhịp (clock edge)**: RTL đồng bộ (synchronous) chỉ cập nhật
  thanh ghi tại cạnh lên (hoặc xuống) của `clk`. Để mô phỏng đúng 1 chu kỳ
  xung nhịp, bạn phải: đặt `clk=0` → `eval()` → đặt `clk=1` → `eval()`
  (hoặc ngược lại, tùy RTL nhạy cạnh nào) — **cả hai bước đều bắt buộc**,
  thiếu một bước là sai hoàn toàn về mặt thời gian mô phỏng.
- **Reset**: thanh ghi trong Verilated model **không có giá trị mặc định
  đáng tin cậy** cho tới khi bạn chủ động đưa `rst_n=0` qua vài cạnh xung
  nhịp rồi thả ra — giống hệt yêu cầu reset khi bật nguồn silicon thật.
  Bỏ qua bước này là nguồn lỗi rất phổ biến (thanh ghi "X" hoặc giá trị
  rác từ lần chạy trước).

Lệnh biên dịch cơ bản:
```bash
verilator --cc npu_mac_core.v --Mdir obj_dir -Wall
```
Sinh ra `obj_dir/Vnpu_mac_core.h` (khai báo class `Vnpu_mac_core` với mọi
port của module là public member: `dut->clk`, `dut->rst_n`, `dut->in_a`...)
và các file `.cpp` cài đặt logic bên trong.

---

## 3. Hai kiến trúc tích hợp RTL vào gem5 <a name="3"></a>

Có hai cách chính, cả hai đều là pattern thật (không phải suy đoán) — cách
thứ hai là hạ tầng **có sẵn, chính thức** trong source code gem5
(`src/systemc/tlm_bridge/`), cách thứ nhất là cách mình thiết kế dựa trên
đúng hạ tầng `AccelBase` đã có của nền tảng này.

| | **A) Direct-in-SimObject** (ví dụ trong tài liệu này) | **B) SystemC/TLM bridge** (hạ tầng có sẵn của gem5) |
|---|---|---|
| RTL cần ở dạng | Verilog thô → tự dịch bằng `verilator --cc` | Verilog → `verilator --sc` (SystemC) hoặc mô hình TLM viết tay |
| Độ phức tạp tích hợp | Thấp — chỉ là một C++ object trong 1 SimObject | Cao hơn — cần hiểu SystemC + TLM 2.0 + `SystemC_Kernel` chạy song song với event queue của gem5 |
| Tái sử dụng hạ tầng AccelBase | Có — chỉ thêm 1 hook nhỏ (`beginCompute()`), giữ nguyên register file/DMA | Không trực tiếp — TLM có mô hình payload/socket riêng, cần bridge (`Gem5ToTlmBridge`/`TlmToGem5Bridge`) |
| Phù hợp khi | RTL nội bộ công ty, có Verilog nguồn, muốn tích hợp nhanh vào accelerator đã có | Nhận model TLM/SystemC sẵn từ đối tác/vendor IP, hoặc cần chuẩn interop TLM với công cụ khác |
| Điểm cần cẩn thận | Bạn tự quản lý việc "lái" RTL đúng thời điểm trong FSM của SimObject | Phải đồng bộ đúng giữa 2 scheduler (SystemC kernel và gem5 event queue) — dễ sai nếu chưa quen |

Tài liệu này trình bày **chi tiết đầy đủ cách A** (mục 4-5, có ví dụ chạy
thật), và **giải thích cách B ở mức đủ để bắt đầu** (mục 6), vì cách B dùng
đúng hạ tầng có sẵn của gem5 nên rủi ro tích hợp thấp hơn về lâu dài nếu RTL
team của bạn đã quen thuộc với SystemC/TLM.

---

## 4. Ví dụ hoạt động: NPU MAC core thật <a name="4"></a>

Để tài liệu này không chỉ là lý thuyết, mình viết một RTL thật nhỏ đại diện
cho "khối mà RTL team giao cho bạn": `rtl/npu_mac_core.v` — một lõi
streaming MAC (nhân-cộng-dồn), giao diện handshake valid/ready giống hầu
hết IP accelerator thật:

```verilog
module npu_mac_core #(
    parameter DATA_W = 16,
    parameter ACC_W  = 32
) (
    input  wire                  clk,
    input  wire                  rst_n,
    input  wire                  start,   // xung bắt đầu 1 lần tính
    input  wire [15:0]           length,  // số cặp (a,b) cho lần này
    input  wire                  in_valid,
    output wire                  in_ready,
    input  wire [DATA_W-1:0]     in_a,
    input  wire [DATA_W-1:0]     in_b,
    output reg                   done,
    output reg  [ACC_W-1:0]      result
);
```
Hành vi: khi `start` được xung, RTL nhận `length` cặp `(in_a, in_b)` theo
handshake `in_valid`/`in_ready` (1 cặp/chu kỳ khi cả hai đều `1`), tính
`result = sum(in_a[i] * in_b[i])`, rồi phát `done=1` trong đúng 1 chu kỳ.
Xem file đầy đủ ở `rtl/npu_mac_core.v` trong phụ lục.

### Đã kiểm chứng bằng testbench độc lập (bước bắt buộc, làm trước khi đụng vào gem5)

**Nguyên tắc quan trọng nhất của toàn bộ tài liệu này**: **luôn luôn xác
minh RTL hoạt động đúng bằng một testbench C++/Verilator độc lập, đứng
riêng, TRƯỚC KHI tích hợp vào gem5.** Nếu RTL sai hoặc cách bạn "lái" nó
(clock/reset/handshake) sai, gỡ lỗi bên trong một hệ thống gem5 đầy đủ khó
hơn rất nhiều so với gỡ lỗi một chương trình C++ nhỏ độc lập. `rtl/test/
standalone_test.cpp` là testbench đó — mình đã biên dịch và chạy thật:

```
$ verilator --cc rtl/npu_mac_core.v --Mdir obj_dir -Wall \
      --exe --build test/standalone_test.cpp -o standalone_test
$ ./obj_dir/standalone_test
expected = 152
rtl_result = 152
cycles_elapsed (this op) = 9
match = YES
```
(`152` là tổng `sum(a[i]*b[i])` của bộ test vector 8 phần tử trong file —
tính tay bằng C++ thường và so khớp với kết quả RTL trả về; `9` chu kỳ =
8 chu kỳ streaming + 1 chu kỳ để RTL chốt `result`/phát `done`, đúng như
thiết kế 2 trạng thái `S_RUN`→`S_DONE` của RTL.)

Đây là bước "hợp đồng" (contract) giữa bạn và RTL: một khi testbench độc
lập đã xác nhận đúng hành vi + đúng cách lái tín hiệu, phần code lái RTL
bên trong SimObject gem5 (mục 5) chỉ là **chuyển nguyên logic của testbench
này** vào trong một hàm callback của gem5, không viết lại từ đầu.

---

## 5. Từng bước tích hợp (Direct-in-SimObject) <a name="5"></a>

### 5.1. Ý tưởng thiết kế

`AccelBase` (đã có sẵn trong nền tảng) chia một thao tác accelerator thành
4 pha: `FETCH` (DMA đọc) → `COMPUTE` → `WRITEBACK` (DMA ghi) → `DONE`. Pha
`FETCH`/`WRITEBACK` dùng `dmaRead()`/`dmaWrite()` thật của gem5 — **không
đổi gì ở đây**, dù compute là phân tích hay RTL. Chỉ pha `COMPUTE` khác:

- **Model phân tích** (`NpuAccel` hiện có): gọi `computeLatency()` **một
  lần** lấy số chu kỳ N, lên lịch **một** sự kiện xảy ra sau N chu kỳ.
- **Model RTL** (`NpuAccelRtl`, mới): lên lịch một sự kiện **lặp lại mỗi
  chu kỳ** (`clockPeriod()`), mỗi lần gọi `eval()` một lần trên Verilated
  model đúng như testbench độc lập đã làm, và tự kiểm tra tín hiệu `done`
  — khi nào `done=1` mới báo cho FSM chuyển sang `WRITEBACK`.

Để hỗ trợ cả 2 kiểu mà không viết lại `AccelBase`, mình thêm **đúng 1 hook
ảo** (`beginCompute()`) — bản vá (diff) rất nhỏ, không ảnh hưởng
`NpuAccel`/`LpuAccel`/`IspAccel` đang chạy:

```cpp
// accel_base.hh — thêm vào phần protected:
virtual void beginCompute();              // MỚI — mặc định dùng computeLatency()
void finishCompute() { onComputeDone(); } // MỚI — subclass RTL gọi khi RTL xong
uint8_t *scratchPtr() const { return scratch.get(); }  // MỚI — cho subclass đọc/ghi buffer
uint64_t scratchLen() const { return lenReg; }         // MỚI

// computeLatency() đổi từ "= 0" (pure virtual) thành virtual thường,
// có implementation mặc định (panic) trong accel_base.cc, để subclass
// RTL không bị bắt buộc override một công thức nó không dùng.
virtual Cycles computeLatency(uint64_t lenBytes, uint64_t param0,
                               uint64_t param1) const;
```
```cpp
// accel_base.cc — onFetchDone() đổi từ tính trực tiếp sang gọi hook:
void AccelBase::onFetchDone() { state = State::Compute; beginCompute(); }

// Implementation mặc định — giữ nguyên hành vi cũ cho NpuAccel/LpuAccel/IspAccel:
void AccelBase::beginCompute() {
    Cycles c = computeLatency(lenReg, param0Reg, param1Reg);
    schedule(computeDoneEvent, curTick() + cyclesToTicks(c));
}
```
Xem file đầy đủ trong phụ lục 9.A — đây là *toàn bộ* thay đổi cần thiết
trên `AccelBase` đã có, không có gì khác.

### 5.2. `NpuAccelRtl` — chuyển đúng logic testbench vào SimObject

So sánh trực tiếp: cột trái là testbench độc lập (đã verify ở mục 4), cột
phải là code SimObject — **cùng một logic**, chỉ khác nguồn dữ liệu (mảng
C++ cứng → buffer DMA thật) và cách lặp (vòng `for` chờ → sự kiện gem5
lên lịch lại chính nó):

```cpp
// testbench (mục 4)                    // NpuAccelRtl::stepCycle() (gem5)
for (...) {                             void NpuAccelRtl::stepCycle() {
  if (idx < a.size()) {                   if (feedIdx < numElems) {
    dut->in_valid = 1;                      rtl->in_valid = 1;
    dut->in_a = a[idx];                     rtl->in_a = data[2*feedIdx];
    dut->in_b = b[idx];                     rtl->in_b = data[2*feedIdx+1];
  } else dut->in_valid = 0;               } else rtl->in_valid = 0;

  tick(dut);   // clk 0->1 + eval x2       rtl->clk=0; rtl->eval();
                                            rtl->clk=1; rtl->eval();

  if (dut->in_valid && dut->in_ready)     if (rtl->in_valid && rtl->in_ready)
    idx++;                                  feedIdx++;

  if (dut->done) { ... break; }           if (rtl->done) {
}                                            ...; finishCompute();
                                          } else {
                                            schedule(stepEvent,
                                              curTick()+clockPeriod());
                                          }
                                          }
```
Đây chính là lý do bước 4 (verify độc lập) quan trọng: một khi bạn tin
tưởng cột trái đúng, việc chuyển sang cột phải chỉ là "đóng gói lại", rủi
ro sai sót giảm đi rất nhiều so với viết logic lái RTL trực tiếp trong
gem5 ngay từ đầu (nơi một lỗi có thể lẫn giữa lỗi RTL, lỗi lái tín hiệu,
và lỗi tích hợp gem5 cùng lúc, rất khó tách bạch).

Điểm cần chú ý về **thời gian (timing)**: mỗi lần `stepCycle()` chạy tương
ứng đúng 1 chu kỳ xung nhịp thật của RTL, và được lên lịch cách nhau đúng
`clockPeriod()` tick của gem5 — nghĩa là `curTick()` của gem5 tiến đúng
theo số chu kỳ RTL thật đã chạy. Vì vậy **không cần** tự tính/ghi
`CYCLES_LAST` thủ công — cơ chế có sẵn của `AccelBase::onWritebackDone()`
(`cyclesLastReg = ticksToCycles(curTick() - opStartTick)`) đã tự động đúng,
vì nó đo trực tiếp từ tick thật đã trôi qua, chứ không phải một công thức.
Đây là điểm khác biệt cốt lõi so với model phân tích: **CYCLES_LAST giờ là
số đo thật, không phải ước lượng.**

### 5.3. Build

Verilator sinh mã C++ theo Makefile riêng của nó, không theo SCons — cách
đơn giản và ít rủi ro nhất là để Verilator tự build ra 1 static library
(`.a`), rồi bảo SCons link vào, thay vì cố quản lý từng file `.cpp` do
Verilator sinh ra (tên file này thay đổi giữa các phiên bản Verilator, rất
dễ vỡ nếu enumerate tay). Xem `rtl/SConscript` (phụ lục 9.C).

```bash
# Cài Verilator (Ubuntu/Debian):
sudo apt install verilator

# Build gem5 kèm demo RTL (mặc định TẮT để không bắt buộc mọi người
# phải cài Verilator chỉ để build các model phân tích NPU/LPU/ISP):
scons build/RISCV/gem5.opt GEM5_RTL_DEMO=1 -j$(nproc)
```

### 5.4. Chạy thử

Đăng ký `NpuAccelRtl` vào config Python giống hệt cách `NpuAccel` đã được
gắn vào `SystemXBar` trong `configs/accel_se_system.py` (cùng `pio`/`dma`
port, cùng convention). Bật `--debug-flags=Accel` để xem log từng bước
FETCH→COMPUTE(RTL)→WRITEBACK, và so `CYCLES_LAST` đọc được với số `9` chu
kỳ testbench độc lập đã đo — nếu khớp, tích hợp đã đúng.

---

## 6. Kiến trúc thay thế: SystemC/TLM bridge (hạ tầng có sẵn của gem5) <a name="6"></a>

Nếu RTL bạn nhận được đã ở dạng **SystemC/TLM** (ví dụ đối tác giao IP dưới
dạng "TLM model", hoặc bạn dùng `verilator --sc` thay vì `--cc`), gem5 đã
có sẵn hạ tầng chính thức để nối vào — không cần tự viết cầu nối như mục 5:

- `src/systemc/tlm_bridge/gem5_to_tlm.hh` → SimObject `Gem5ToTlmBridge32`
  (và `64/128/256/512`): phía gem5 có một `ResponsePort` (nhận lệnh CPU/DMA
  như một `PioDevice` bình thường), phía SystemC có một TLM initiator
  socket gọi `b_transport()` vào module SystemC/TLM của bạn. Dùng khi RTL
  (dưới dạng TLM) đóng vai trò **target** (ví dụ RTL là accelerator nhận
  lệnh điều khiển).
- `src/systemc/tlm_bridge/tlm_to_gem5.hh` → `TlmToGem5Bridge32/64/...`:
  chiều ngược lại — RTL/TLM đóng vai trò **initiator** phát traffic DMA
  vào hệ thống gem5 (ví dụ RTL tự đọc/ghi bộ nhớ chính).
- `SystemC_Kernel` (SimObject gắn ở `Root`): chạy scheduler SystemC song
  song, đồng bộ với event queue của gem5 — đây là phần "phép màu" giúp 2
  hệ thống sự kiện độc lập (SystemC và gem5) tiến thời gian cùng nhau.

Ví dụ chính thức đầy đủ (đã có sẵn trong mọi checkout gem5, không cần viết
lại) nằm ở `util/systemc/systemc_within_gem5/systemc_gem5_tlm/` — gồm
`sc_tlm_target.hh/cc` (một `SC_MODULE` mẫu đóng vai trò target) và
`config.py` (cách nối `Gem5ToTlmBridge32` với module đó qua
`system.transactor.tlm = system.target.tlm`). Build bằng cơ chế `EXTRAS`
của SCons:
```bash
scons build/RISCV/gem5.opt \
    EXTRAS=util/systemc/systemc_within_gem5/systemc_gem5_tlm \
    setconfig USE_SYSTEMC=y
scons build/RISCV/gem5.opt -j$(nproc)
```
(`USE_SYSTEMC` là một Kconfig symbol thật của gem5, định nghĩa ở
`src/systemc/Kconfig`, mặc định `y` nếu môi trường build có SystemC —
gem5 đã đóng gói sẵn SystemC 2.3.1 trong `ext/systemc`, không cần cài đặt
SystemC ngoài.)

**Khi nào chọn B thay vì A**: nếu team RTL của bạn đã quen SystemC/TLM
(chuẩn công nghiệp phổ biến cho trao đổi model IP giữa các công ty), hoặc
bạn cần accelerator vừa là target (nhận lệnh) vừa là initiator (tự làm
DMA) một cách chuẩn hoá — TLM có sẵn khái niệm cho cả hai vai trò với
protocol thống nhất. Đánh đổi là độ phức tạp học tập ban đầu cao hơn
(TLM 2.0 socket, payload, phase, 2 scheduler chạy song song).

---

## 7. Mức độ đã kiểm chứng của tài liệu này <a name="7"></a>

Để bạn biết chính xác nên tin phần nào và tự kiểm tra lại phần nào:

| Thành phần | Mức kiểm chứng |
|---|---|
| `rtl/npu_mac_core.v` | **Đã verify** — biên dịch bằng Verilator 5.020 thật, chạy testbench độc lập, kết quả tính đúng (152, khớp tính tay), đo được số chu kỳ thật (9) |
| `rtl/test/standalone_test.cpp` | **Đã verify** — chính là testbench dùng để kiểm tra RTL ở trên, build+run thành công |
| Diff `accel_base.hh/cc` (hook `beginCompute()`) | Thiết kế theo đúng pattern đã dùng cho `NpuAccel`/`LpuAccel`/`IspAccel` (đã build test thành công ở phần trước của dự án), nhưng **chưa build lại toàn bộ gem5 sau khi thêm diff này** trong môi trường hiện tại (build gem5 đầy đủ mất 30-60+ phút) |
| `NpuAccelRtl.hh/cc/py` | Logic lái RTL **chuyển trực tiếp** từ testbench đã verify (mục 5.2), nhưng **chưa build/test trong gem5 thật** — rủi ro còn lại chủ yếu ở việc gọi API SimObject (tên hàm `clockPeriod()`, `schedule()`, kiểu `Cycles`...) có khớp tuyệt đối với phiên bản gem5 bạn dùng hay không |
| `rtl/SConscript` (build integration) | Thiết kế hợp lý dựa trên khả năng thật của SCons (`env.Command`, `env.Append`), nhưng **là phần rủi ro cao nhất trong toàn bộ tài liệu** — tích hợp build system bên thứ 3 (Verilator) vào SCons là việc dễ vỡ ở chi tiết (tên file Makefile của Verilator có thể khác giữa các phiên bản). Đã thấy Verilator 5.020 thật sinh ra `Vnpu_mac_core__ALL.a` qua `make -f Vnpu_mac_core.mk Vnpu_mac_core__ALL.a` trong quá trình test standalone — SConscript dùng đúng tên target đó, nhưng khuyến nghị bạn tự chạy thử bước build này riêng (`scons build/RISCV/gem5.opt GEM5_RTL_DEMO=1`) và đối chiếu lỗi (nếu có) trước khi mở rộng thêm |
| Mục 6 (SystemC/TLM bridge) | Toàn bộ tên class/API (`Gem5ToTlmBridge32`, `SystemC_Kernel`, `USE_SYSTEMC`...) **đã xác nhận tồn tại thật** bằng cách đọc trực tiếp source code gem5 nhánh `stable` (`src/systemc/tlm_bridge/`, `src/systemc/Kconfig`, `util/systemc/systemc_within_gem5/`) — đây là hạ tầng chính thức, không phải suy đoán, nhưng ví dụ cụ thể chưa được build/chạy trong môi trường này |

**Khuyến nghị quy trình khi bạn áp dụng thật**: (1) chạy lại đúng bước 4
(testbench độc lập) cho RTL thật của bạn trước tiên — đây là bước rẻ và
nhanh nhất để bắt lỗi; (2) áp diff `AccelBase` + build thử
`scons build/RISCV/gem5.opt` (không cần `GEM5_RTL_DEMO=1`) để chắc chắn
diff không phá model phân tích hiện có; (3) mới bật `GEM5_RTL_DEMO=1` và
xử lý lỗi build Verilator/SCons nếu có, dùng thông báo lỗi cụ thể để tra
cứu tiếp (SCons + Verilator kết hợp là tổ hợp ít tài liệu công khai nhất
trong toàn bộ hướng dẫn này).

---

## 8. Cạm bẫy kỹ thuật thường gặp <a name="8"></a>

1. **Quên gọi `eval()` sau khi đổi input** — RTL sẽ dùng giá trị input cũ,
   kết quả sai một cách khó hiểu (không crash, chỉ sai số).
2. **Chỉ đổi `clk` một chiều** (chỉ `0→1` mà không có bước `1→0` trước đó
   trong cùng 1 chu kỳ) — với thiết kế nhạy cả 2 cạnh hoặc có logic tổ hợp
   phụ thuộc mức, dễ bỏ sót sự kiện.
3. **Bỏ qua reset** — thanh ghi Verilated model khởi tạo không xác định,
   kết quả lần chạy đầu tiên có thể đúng "tình cờ" rồi sai ở lần sau (do
   trạng thái global còn sót lại) — luôn luôn reset tường minh trước mỗi
   lần dùng lại 1 instance.
4. **Nhầm lẫn giữa "1 chu kỳ RTL" và "1 tick gem5"** — nếu bạn lên lịch sự
   kiện cách nhau sai số tick (ví dụ dùng hằng số cứng thay vì
   `clockPeriod()` thật của `clk_domain` accelerator), số liệu `CYCLES_LAST`
   sẽ sai dù RTL chạy đúng. Luôn dùng `clockPeriod()`/`cyclesToTicks()` của
   chính SimObject, không hard-code.
5. **Endianness/kích thước dữ liệu giữa buffer DMA và port RTL** — buffer
   DMA là mảng byte thô; nếu RTL port là `int16_t`/`int32_t`, cách bạn
   `reinterpret_cast` phải khớp đúng little-endian (RISC-V mặc định) với
   thứ tự byte RTL mong đợi — sai chỗ này cho kết quả "gần đúng nhưng không
   đúng" rất khó phát hiện nếu không có testbench độc lập để so sánh số.
6. **Không có giới hạn an toàn (guard) cho vòng lặp chờ `done`** — nếu RTL
   có bug (không bao giờ phát `done`), SimObject sẽ treo vô hạn. Testbench
   độc lập nên luôn có `guard` như `standalone_test.cpp` (phụ lục), và
   cân nhắc thêm timeout tương tự (kèm `panic()`) trong `stepCycle()` khi
   đưa vào production.
7. **Verilator sinh ra tên file/target Makefile khác nhau giữa các phiên
   bản** — nếu `rtl/SConscript` báo lỗi "no rule to make target", chạy tay
   `verilator --cc ... --Mdir obj_dir && ls obj_dir/*.mk` và tự đối chiếu
   tên target thật với `Vnpu_mac_core.mk` để sửa `rtl/SConscript` cho khớp
   phiên bản Verilator bạn cài.

---

## 9. Phụ lục: toàn bộ code tham chiếu <a name="9"></a>

### 9.A. Diff đầy đủ trên `AccelBase` (đã áp dụng vào `src/accelerators/accel_base.hh` và `.cc` trong package chính)

Xem file thật tại `src/accelerators/accel_base.hh` / `accel_base.cc` —
phần thay đổi nằm ở khối `protected:` (hook `beginCompute()`,
`finishCompute()`, `scratchPtr()`, `scratchLen()`) và hàm `onFetchDone()`.

### 9.B. `NpuAccelRtl` — file đầy đủ

- `src/accelerators/npu_accel_rtl.hh`
- `src/accelerators/npu_accel_rtl.cc`
- `src/accelerators/NpuAccelRtl.py`

### 9.C. RTL + build + test

- `src/accelerators/rtl/npu_mac_core.v` — RTL đã verify (mục 4)
- `src/accelerators/rtl/test/standalone_test.cpp` — testbench đã verify
- `src/accelerators/rtl/SConscript` — build integration (rủi ro cao nhất, xem mục 7)

### 9.D. Lệnh verify độc lập (chạy lại bất cứ lúc nào để tự tin trước khi đụng gem5)

```bash
cd src/accelerators/rtl
verilator --cc npu_mac_core.v --Mdir obj_dir -Wall \
    --exe --build test/standalone_test.cpp -o standalone_test
./obj_dir/standalone_test
# Kỳ vọng: "match = YES" và một số chu kỳ hợp lý in ra màn hình.
```
