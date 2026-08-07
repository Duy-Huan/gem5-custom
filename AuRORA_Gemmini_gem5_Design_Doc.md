# AuRORA-Gemmini trên gem5 — Tài liệu thiết kế (Design Document)

**Phiên bản:** 1.0
**Đối tượng đọc:** Kỹ sư gem5 (C++), kỹ sư firmware/runtime, kỹ sư RTL làm co-design
**Mục tiêu tài liệu:** Đủ chi tiết để 1 kỹ sư gem5 chưa từng đọc RTL AuRORA/Gemmini có thể bắt đầu code SimObject ngay, và 1 kỹ sư firmware có thể viết driver điều khiển đúng ngay từ lần đầu.

**Nguồn tham chiếu chính (đã grep trực tiếp mã nguồn, không suy đoán):**
| Repo | Vai trò | Commit tham chiếu |
|---|---|---|
| [`ucb-bar/rerocc`](https://github.com/ucb-bar/rerocc) | RTL + firmware chuẩn của giao thức AuRORA (Client/Manager) | main, snapshot 2026-08-04 |
| [`ucb-bar/gemmini`](https://github.com/ucb-bar/gemmini) | RTL accelerator (Gemmini) | main, cùng ngày |
| [`chipsalliance/rocket-chip`](https://github.com/chipsalliance/rocket-chip) | CPU core (nơi xử lý `stall`/`fence` mà rerocc dùng) | main, cùng ngày |
| [`ucb-bar/chipyard`](https://github.com/ucb-bar/chipyard) | SoC integration framework | main, cùng ngày |
| [`gem5/gem5`](https://github.com/gem5/gem5) | Nền tảng mô phỏng đích | main, cùng ngày |
| Kim et al., *"AuRORA: A Full-Stack Solution for Scalable and Virtualized Accelerator Integration"*, MICRO 2023 | Kiến trúc tổng quan, ý tưởng thiết kế | — |

**Quy ước ký hiệu trong tài liệu:**
- 🔧 = hướng dẫn implement cụ thể cho gem5
- ⚠️ = điểm cần cẩn trọng / sai khác giữa các nguồn
- 📖 = trích dẫn RTL/paper chính xác (file:line)
- ❓ = quyết định thiết kế còn mở, cần chốt trước khi code

---

## Mục lục

1. [Tổng quan hệ thống](#1-tổng-quan-hệ-thống)
2. [System Architecture — chi tiết từng khối](#2-system-architecture--chi-tiết-từng-khối)
3. [Design Specification — đặc tả cho việc code SimObject](#3-design-specification--đặc-tả-cho-việc-code-simobject)
4. [User Guide — Firmware API](#4-user-guide--firmware-api)
5. [Sequence Diagram — diễn giải chi tiết từng bước](#5-sequence-diagram--diễn-giải-chi-tiết-từng-bước)
6. [Phụ lục](#6-phụ-lục)

---

## 1. Tổng quan hệ thống

### 1.1 AuRORA là gì

AuRORA (paper MICRO'23) là kiến trúc **tách rời (disaggregate) accelerator khỏi CPU core**, cho phép nhiều CPU core chia sẻ động 1 pool accelerator vật lý thông qua giao thức **ReRoCC (Remote RoCC)**. Thay vì mỗi CPU gắn cứng 1 accelerator riêng (mô hình RoCC truyền thống), AuRORA đưa accelerator ra xa CPU, kết nối qua Network-on-Chip (NoC), và cho phép **acquire/release động theo runtime** — giống mô hình virtual memory nhưng áp dụng cho accelerator.

### 1.2 5 khối chính của hệ thống

```
┌─────────────┐     ┌──────────────┐     ┌─────────┐     ┌──────────────┐     ┌───────────┐
│  RISC-V CPU  │◄───►│AuRORA Client │◄───►│   NoC   │◄───►│AuRORA Manager│◄───►│  Gemmini  │
│ (rocket-chip)│     │  (Client.scala)     │(Garnet/ │     │(Manager.scala)     │(Controller│
└─────────────┘     └──────────────┘     │ xbar)   │     └──────────────┘     │  .scala)  │
                                          └─────────┘            │                └───────┘
                                                                  ▼
                                                          ┌───────────────┐
                                                          │ SoC Interconnect│
                                                          │  → L2 → DDR    │
                                                          └───────────────┘
```

- **RISC-V CPU**: core chạy firmware, phát lệnh RoCC custom + ghi/đọc CSR điều khiển.
- **AuRORA Client**: shim gắn trực tiếp vào CPU, quản lý acquire/release/credit/routing lệnh tới Manager đúng qua NoC.
- **NoC**: mạng kết nối Client ↔ Manager (và nhiều Client/Manager khác trong hệ thống multi-tenant).
- **AuRORA Manager**: shim bọc quanh accelerator, dịch địa chỉ (MMU riêng), forward lệnh RoCC chuẩn vào accelerator.
- **Gemmini**: accelerator ma trận thật sự thực thi.

### 1.3 Nguyên lý cốt lõi cần nhớ trước khi đọc tiếp

1. **Client và Manager giao tiếp thuần qua NoC bằng 1 giao thức 6-opcode** (`mAcquire/mInst/mUStatus/mUPtbr/mRelease/mUnbusy` chiều đi, `sAcqResp/sInstAck/sWrite/sRelResp/sUnbusyAck` chiều về) — **không có kết nối trực tiếp Manager↔CPU**.
2. **Acquire/Release/Fence làm CPU core STALL TOÀN BỘ** (xác nhận qua `rocket-chip/CSR.scala` + `RocketCore.scala`) — khác hẳn lệnh accelerator thường (`mInst`) vốn **non-blocking**.
3. **Gemmini hầu như không bao giờ trả kết quả qua đường RoCC response** — kết quả nằm trong Accumulator on-chip, phải `mvout` + `fence` mới lấy ra được.
4. **2 tầng TLB độc lập**: Gemmini FrontendTLB (tầng 1, nhỏ) → Manager PTW/L2TLB (tầng 2, mặc định TẮT).

---

## 2. System Architecture — chi tiết từng khối

> Tham chiếu trực tiếp 4 file: `RoccCPU_sa.puml`, `client_sa.puml`, `manager_sa.puml`, `gemmini_sa.puml`

### 2.1 Khối RoCC CPU + AuRORA Client

**Chức năng**: Client là lớp trung gian giữa CPU và NoC. Nó quản lý:
- **16 Tracker slot** (`cfg[acq_bit | mgr_id]`, CSR `RRCFG0-15`) — mỗi slot đại diện cho 1 accelerator đã/đang được acquire.
- **4 slot decode-opcode** (`RROPC0-3`) — ánh xạ 1 trong 4 mã lệnh custom RISC-V (`custom0-3`) sang 1 trong 16 Tracker slot, giải quyết bài toán không gian mã lệnh giới hạn.
- **16 credit pool độc lập** (mỗi pool sâu 4, theo từng Tracker slot) — flow-control cho lệnh `mInst` gửi đi.
- **1 FSM điều khiển dùng chung** (`cfg_acq_state`) cho toàn bộ thao tác acquire/release/status-sync/ptbr-sync trên cả 16 slot — đây là điểm nghẽn tiềm ẩn (head-of-line blocking) cần lưu ý khi thiết kế multi-tenant.

**Sub-block bên trong** (theo `client_sa.puml`, đã sửa để khớp RTL):

| Sub-block | Vai trò | Nguồn RTL |
|---|---|---|
| Command Buffer | Đệm lệnh RoCC thô từ CPU pipeline | `Client.scala` (io.cmd) |
| RoCC Decoder & Packet Formatter | Giải mã opcode field, tra `RROPCn`→cfg_id, đóng gói thành message NoC | `Client.scala:198-211` |
| Per-cfg Credit Pools | 16 counter độc lập, mỗi counter init=4 | `Client.scala:89` |
| Shared Control FSM | 8-state (`s_idle/s_acq/s_acq_ack/s_rel/s_rel_ack/s_status0/s_status1/s_ptbr`) | `Client.scala:107` |
| Response Unpacker & FIFO | Nhận `sWrite` 2-beat, ghép lại `{rd, data}` | `Client.scala:236-247` |
| Register File Arbiter | Ghi kết quả vào regfile CPU | — |

**Port ngoài** (đã chuẩn hoá theo API gem5):

| Port | Loại gem5 | Hướng | Nối tới |
|---|---|---|---|
| `P_ROCC_NOC` | `RequestPort` (1 object, 2 chiều) | Client → NoC | Manager's `P_NOC_REQ`/`P_NOC_RESP` |
| `P_CPU_PIPE` | (nội bộ core, không phải gem5 Port — xem §3.2) | CPU → Client | — |
| `P_CPU_STALL` | tín hiệu freeze pipeline nội bộ | Client → CPU | — |

⚠️ **Lưu ý quan trọng**: Ban đầu tài liệu có 2 file mô tả gần giống nhau (`RoccCPU_sa.puml` lồng CPU+Client, `client_sa.puml` tách riêng Client) từng dùng **2 port riêng** (`P_ROCC_CMD`/`P_ROCC_RESP`) — đã sửa lại thành **1 `RequestPort` duy nhất**, đúng với việc `RequestPort` trong gem5 (`src/mem/port.hh`) tự xử lý cả `sendTimingReq()` lẫn `recvTimingResp()`.

### 2.2 Khối AuRORA Manager

**Chức năng**: Manager là shim bọc quanh 1 accelerator vật lý (ở đây là Gemmini). Nó:
- Nhận message ReRoCC từ NoC, giải mã 6 opcode.
- Duy trì FSM 5-state (`s_idle/s_active/s_rel_wait/s_sfence/s_unbusy`) theo dõi trạng thái acquire.
- Giữ **Shadow Registers** (bản sao `status`/`ptbr` của CPU đang acquire) để accelerator dịch địa chỉ đúng ngữ cảnh.
- Đệm lệnh vào `inst_q` (FIFO sâu 4) trước khi forward tới accelerator's `io.cmd`.
- Sở hữu **MMU nội bộ** (PTW + tùy chọn L2TLB/PTECache, mặc định TẮT — 0 entries).

**Sub-block** (theo `manager_sa.puml`, đã sửa):

| Sub-block | Vai trò |
|---|---|
| Instruction Buffer (`inst_q`) | FIFO depth=4, đệm lệnh chờ accelerator sẵn sàng |
| RoCC Command Decoder | Giải mã 6 opcode ReRoCC |
| Tenant Context Manager | Giữ shadow `status`/`ptbr`, cập nhật qua `mUStatus`(2-beat)/`mUPtbr`(1-beat) |
| Fence & Status Monitor | Sample `io.busy` của accelerator cho cơ chế fence/release |
| SFENCE/Flush Coordinator | Trigger `PTW.sfence.valid` **nội bộ**, tự động khi release hoàn tất |
| **MMU Subsystem** (`Mgr_MMU`) | PTW + L2TLB/PTECache — **submodule nội bộ**, KHÔNG phải external port |
| Writeback Arbiter | Forward `io.resp` của accelerator (nếu có) thành `sWrite` |

**Port ngoài** (đã sửa lỗi nghiêm trọng — xem ⚠️ dưới):

| Port | Loại gem5 | Hướng | Nối tới |
|---|---|---|---|
| `P_NOC_REQ` | `ResponsePort` | NoC → Manager | Client (nhận 6 opcode) |
| `P_NOC_RESP` | (cùng 1 `ResponsePort`, chiều phản hồi) | Manager → NoC | Client |
| `P_GEM_CMD` | không phải gem5 Port — xem §3.4 | Manager → Gemmini | `io.cmd` |
| `P_GEM_BUSY` | tương tự | Gemmini → Manager | `io.busy` |
| `P_GEM_RESP` | tương tự | Gemmini → Manager | `io.resp` |
| `P_SYS_MEM` | `RequestPort` | Manager → SoC Interconnect | `tlNode`, dùng chung cho PTW walk + traffic accelerator |

⚠️ **LỖI ĐàSỬA (nghiêm trọng nhất tìm thấy trong review)**: Bản vẽ gốc có 2 port `"Host RoCC Cmd (Từ CPU Core)"` / `"Host Resp (Tới CPU rd)"` — vẽ Manager **nối thẳng tới CPU**. Đây là sai bản chất kiến trúc: "**Re**-RoCC" nghĩa là **tách rời** Manager khỏi CPU. Manager chỉ có **1 port hướng NoC** (`P_NOC_REQ`/`P_NOC_RESP`), không có port nào riêng cho "CPU". Nhiều Client khác nhau (thuộc nhiều CPU khác nhau) có thể lần lượt acquire cùng 1 Manager theo thời gian — nếu code cứng 1 kết nối Manager↔CPU sẽ phá vỡ hoàn toàn khả năng multi-tenant.

### 2.3 Khối Gemmini (Accelerator)

**Chức năng**: thực thi phép nhân ma trận/tích chập qua systolic array, có scratchpad+accumulator on-chip riêng.

**Sub-block chính** (theo `gemmini_sa.puml`):

| Sub-block | Vai trò | Default config |
|---|---|---|
| Cmd Queue (`raw_cmd_q`) | Đệm lệnh thô đầu vào | depth=2 |
| LoopUnroller (`LoopMatmul`/`LoopConv`) | Giải nén mega-instruction `LOOP_WS*`/`LOOP_CONV_WS*` thành lệnh nguyên thuỷ | — |
| Reservation Station (= "ROB" trong docs Chipyard) | Scoreboard 3 hàng đợi song song `ld_q/ex_q/st_q` + dependency bitvector | ld=8, ex=16, st=4 (RS vật lý=48) |
| Load/Store/Ex Controller | Issue lệnh đã sẵn sàng (hết dependency) tới DMA/Mesh | — |
| FrontendTLB (L1) | TLB nội bộ Gemmini, dùng chung Load+Store | `tlb_size=4`, `use_shared_tlb=true` |
| DMA (XactTracker + BeatPacker) | Giao tiếp TileLink, nhiều outstanding request | `num_ids=32` |
| Scratchpad/Accumulator | SRAM on-chip lưu trữ trung gian + kết quả | 256KB/4banks (Scratchpad), 64KB (Accumulator) |
| Mesh (Ex Controller) | Systolic array PE thực thi MAC | 16×16, weight-stationary |

**Port ngoài**:

| Port | Loại gem5 | Hướng | Nối tới |
|---|---|---|---|
| `P_CMD` | tương tự RoCC bundle, không phải Port chuẩn | Manager → Gemmini | `io.cmd` |
| `P_BUSY` | tương tự | Gemmini → Manager | `io.busy` |
| `P_RESP` | tương tự | Gemmini → Manager | `io.resp` (chỉ `COUNTER_OP`) |
| `P_PTW` | **cần adapter**, xem §3.4 | Gemmini → Manager's MMU | `io.ptw` (miss ở tầng 1) |
| `P_MEM` | `RequestPort` chuẩn | Gemmini → SoC Interconnect | `io.mem` (TileLink DMA) |

✅ `P_MEM` là chuỗi kết nối **hoàn toàn chuẩn gem5**, tái dùng được 100%: `RequestPort → SystemXBar/CoherentXBar → Cache(L2) → MemCtrl+DRAMInterface(DDR)`, không cần code mới.

### 2.4 Bảng tổng hợp toàn bộ port hệ thống

| # | Port | Khối sở hữu | Loại gem5 khuyến nghị | Trạng thái |
|---|---|---|---|---|
| 1 | `P_ROCC_NOC` | Client | `RequestPort` | ✅ Đã chuẩn hoá |
| 2 | `P_NOC_REQ`/`P_NOC_RESP` | Manager | `ResponsePort` | ✅ Đã sửa (bỏ port giả CPU) |
| 3 | `P_GEM_CMD`/`BUSY`/`RESP` | Manager↔Gemmini | ❓ Xem §3.4 — khuyến nghị **không dùng Port**, dùng con trỏ C++ trực tiếp | ❓ Cần chốt |
| 4 | `P_PTW` | Gemmini→Manager MMU | Custom, tái dùng `arch::riscv::Walker` + adapter | ❓ Cần chốt adapter |
| 5 | `P_SYS_MEM` (Manager) | Manager | `RequestPort` | ✅ Chuẩn |
| 6 | `P_MEM` (Gemmini) | Gemmini | `RequestPort` | ✅ Chuẩn, tái dùng thư viện gem5 sẵn có |

---

## 3. Design Specification — đặc tả cho việc code SimObject

### 3.1 Danh sách SimObject cần tạo

```
AuroraClient      : ClockedObject      # gắn vào 1 CPU (TimingSimpleCPU hoặc tương đương)
AuroraManager     : ClockedObject      # bọc quanh 1 Gemmini instance
GemminiAccel      : ClockedObject      # accelerator model (có thể chia nhỏ hơn nếu cần độ chi tiết)
AuroraNoC         : SimpleXBar hoặc Bridge (tái dùng gem5 sẵn có — xem §3.3)
```

🔧 Mỗi SimObject nên kế thừa `ClockedObject` (không phải `SimObject` trần) để có sẵn khái niệm chu kỳ đồng hồ, cần thiết cho việc tính latency chính xác theo `cyclesToTicks()`.

### 3.2 ❓ Quyết định thiết kế #1: Mô hình hoá CSR (`RRCFG`/`RROPC`/`RRBAR`)

Đã phân tích 2 phương án ở phần trước, **tóm tắt khuyến nghị**:

| Tiêu chí | CSR thật (khuyến nghị nếu cần validate với RTL) | PIO/MMIO (khuyến nghị nếu ưu tiên tốc độ phát triển) |
|---|---|---|
| Cách làm | Hook `isa.hh:145-147` (`getCSRDataMap`/`getCSRMaskMap`), dùng `IsSerializeAfter` có sẵn | Dùng `BasicPioDevice` (`src/dev/io_device.hh:147`), map `RRCFG0-15`/`RROPC0-3`/`RRBAR` vào 1 vùng địa chỉ vật lý |
| Firmware `rerocc.h` chạy nguyên bản? | ✅ Có | ❌ Không, phải viết lại dùng con trỏ MMIO |
| Tái dùng "block chờ external SimObject" có sẵn của gem5? | ❌ Phải tự viết state mới trong ISA handler | ✅ Tái dùng `DcacheWaitResponse` (`base.hh:119`) |
| Effort | Cao | Thấp |

**❓ Bạn cần chốt phương án trước khi bước sang §3.4.** Tài liệu này viết tiếp theo **giả định chọn CSR thật** (để giữ khả năng chạy `rerocc.h` gốc phục vụ validate chéo với RTL simulation) — nếu chọn PIO, thay mọi chỗ `csrrw`/`csrr` bằng `sw`/`lw` vào vùng địa chỉ MMIO tương ứng, logic backend không đổi.

🔧 **Cách hook CSR cụ thể**:
```cpp
// Trong lớp ISA riêng (kế thừa RiscvISA), override:
const std::unordered_map<int, CSRMetadata>& getCSRDataMap() const override {
    // Thêm entry cho RRCFG0-15 (0x810-0x81f), RROPC0-3 (0x800-0x803), RRBAR (0x804)
    // Mỗi entry trỏ tới hàm đọc/ghi thật sự forward request tới AuroraClient SimObject
}
```
Với ghi CSR `RRCFGn`: gọi hàm `AuroraClient::handleAcquireWrite(cfg_id, wdata)` — hàm này **phải trả về tín hiệu yêu cầu core stall** nếu `cfg_acq_state != s_idle` (xem §5.1 bước 1 để biết chính xác điều kiện). Cách làm gem5-idiomatic nhất: dùng cơ chế tương tự TLB-miss — CPU model gọi vào AuroraClient, nếu chưa sẵn sàng thì trả về 1 "Fault" đặc biệt yêu cầu instruction replay ở tick sau, tương tự cách `DTBWaitResponse` hoạt động cho load/store thường.

### 3.3 Quyết định thiết kế #2: NoC = classic port + xbar (ĐÃ CHỐT)

Không dùng Garnet/Ruby/SLICC (đòi hỏi viết 1 protocol SLICC riêng, effort rất lớn). Dùng:

```
AuroraClient::P_ROCC_NOC ──► SimpleXBar (hoặc Bridge nếu chỉ 1-1) ──► AuroraManager::P_NOC_REQ/RESP
```

🔧 `SimpleXBar` (`src/mem/xbar.hh`) hỗ trợ sẵn nhiều Client cùng nối vào 1 xbar rồi phân phối tới nhiều Manager — đúng mô hình multi-tenant N-Client × M-Manager mà không cần code thêm gì cho phần định tuyến. Latency NoC được xấp xỉ qua tham số `frontend_latency`/`forward_latency` của `SimpleXBar`, không mô phỏng router/VC/hop-by-hop thật như Garnet — **đánh đổi đã được bạn chấp nhận ở lượt trước.**

### 3.4 Giao thức message ReRoCC — đặc tả chính xác từng bit

📖 Nguồn: `bus/Protocol.scala`, `bus/Parameters.scala`

```
Message bundle: { opcode(3 bit), client_id(clientIdBits), manager_id(managerIdBits), data(64 bit) }
MAX_BEATS = 3

Chiều Client → Manager (m*):
  mAcquire  = 0   // 1 beat.  data = don't care (client_id/manager_id trong header)
  mInst     = 1   // 1-3 beat: beat0=inst encoding, beat1=rs1(nếu xs1), beat2=rs2(nếu xs2)
  mUStatus  = 2   // 2 beat: beat0=mstatus[63:0], beat1=mstatus[127:64]
  mUPtbr    = 3   // 1 beat: ptbr
  mRelease  = 4   // 1 beat
  mUnbusy   = 5   // 1 beat (dùng cho fence)

Chiều Manager → Client (s*):
  sAcqResp   = 0  // data(0) = granted bit
  sInstAck   = 1  // ack khi lệnh được DEQUEUE từ inst_q sang io.cmd của accelerator
                  // (KHÔNG PHẢI lúc enqueue — xem §5.1 bước 4, chi tiết quan trọng nhất)
  sWrite     = 2  // 2 beat: beat0=data, beat1=rd — CHỈ gửi nếu accelerator tự assert io.resp.valid
  sRelResp   = 3  // gửi sau khi accelerator drain xong (busy=0, inst_q rỗng)
  sUnbusyAck = 4  // gửi sau khi accelerator drain xong (dùng cho fence)
```

🔧 Định nghĩa 1 `enum class ReRoCCOpcode : uint8_t` dùng chung cho cả AuroraClient và AuroraManager, tránh magic number rải rác.

### 3.5 CSR map — đặc tả bit-chính xác

📖 Nguồn: `client/CSRs.scala`

| CSR | Địa chỉ | Bit width | Layout |
|---|---|---|---|
| `RROPC0` | `0x800` | log2(16)=4 bit | cfg_id |
| `RROPC1` | `0x801` | 4 bit | cfg_id |
| `RROPC2` | `0x802` | 4 bit | cfg_id |
| `RROPC3` | `0x803` | 4 bit | cfg_id |
| `RRBAR` | `0x804` | 4 bit | cfg_id (dùng cho fence) |
| `RRCFG0`...`RRCFG15` | `0x810`...`0x81f` | 9 bit | bit[8]=`acq`, bit[7:0]=`mgr_id` |

### 3.6 State machine — AuroraClient (control-plane, DÙNG CHUNG cho 16 cfg slot)

📖 Nguồn: `Client.scala:107` (8 state)

```
States: s_idle → s_acq → s_acq_ack → s_idle           (chu trình acquire)
        s_idle → s_rel → s_rel_ack → s_idle            (chu trình release)
        s_idle → s_status0 → s_status1 → s_ptbr → s_idle  (auto-resync sau acquire thành công)
```

🔧 Implement như 1 `enum class` + 1 hàm `tick()` cập nhật state mỗi chu kỳ, y hệt mô hình FSM RTL. **Chỉ 1 giao dịch chạy tại 1 thời điểm** trên toàn bộ 16 cfg — nghĩa là `AuroraClient` chỉ cần **1 biến state duy nhất** cho cả 16 slot (không phải mảng 16 state riêng).

⚠️ **Head-of-line blocking đã xác nhận** (`Client.scala:204,206`): khi `cfg_acq_state != s_idle`, việc phát lệnh `mInst` cho **BẤT KỲ** cfg nào khác cũng bị chặn — dù cfg đó hoàn toàn không liên quan tới giao dịch đang chờ. Implement đúng bug/feature này (nó là hành vi RTL thật, không phải lỗi) bằng cách gate điều kiện gửi `mInst` với `cfg_acq_state == s_idle` trên TOÀN client, không chỉ trên cfg đang dùng.

### 3.7 State machine — AuroraManager

📖 Nguồn: `Manager.scala:48`

```
States: s_idle → s_active                              (acquire thành công)
        s_active → s_rel_wait → s_sfence → s_idle       (release, drain xong mới sfence)
        s_active → s_unbusy → s_active                  (fence, không đổi ACQ ownership)
```

Điều kiện chuyển `s_rel_wait → s_sfence`: `!io.busy(accelerator) && inst_q.count == 0` (đã drain hết).
Tại `s_sfence`: trigger `Walker::sfence()` (nếu tái dùng gem5 `arch::riscv::Walker`) để xoá sạch L2TLB/PTECache — **đây là cơ chế cách ly bảo mật đa-tenant bắt buộc**, không được bỏ qua khi implement.

### 3.8 Credit / Flow control — đặc tả chính xác timing

📖 Nguồn: `Manager.scala:154-161` (đã xác nhận qua nhiều vòng review, đây là điểm dễ implement sai nhất)

```cpp
// SAI (dễ mắc phải nếu đọc lướt):
// onReceiveMInst() { instQueue.push(cmd); sendSInstAck(); }   ← credit trả quá sớm!

// ĐÚNG:
void AuroraManager::onReceiveMInst(Cmd cmd) {
    instQueue.push(cmd);   // chỉ enqueue, CHƯA gửi ack
}

void AuroraManager::tryForwardToAccelerator() {
    if (!instQueue.empty() && gemmini->cmdPortReady()) {
        Cmd cmd = instQueue.pop();
        gemmini->sendCmd(cmd);      // forward thật sự
        sendSInstAck(cmd.cfgId);    // ACK CHỈ Ở ĐÂY, đồng thời với forward
    }
}
```

⚠️ Nếu code sai thứ tự này, mô hình gem5 sẽ **đánh giá thấp backpressure thật**, dẫn tới số liệu throughput/latency sai lệch so với silicon thật — đây là lỗi nghiêm trọng nhất từng tìm thấy trong toàn bộ quá trình review sequence diagram.

### 3.9 Link Manager ↔ Gemmini — khuyến nghị KHÔNG dùng gem5 Port

`io.cmd`/`io.resp`/`io.busy` là quan hệ **1-1 cố định**, đồng bộ, không qua NoC/trọng tài nhiều bên. 🔧 Khuyến nghị:

```cpp
class AuroraManager : public ClockedObject {
    GemminiAccel *gemmini;  // con trỏ trực tiếp, set trong Python config qua SimObjectParam
    // Gọi trực tiếp: gemmini->pushCmd(cmd), gemmini->isBusy(), gemmini->popResp()
    // Lên lịch độ trễ bằng EventFunctionWrapper thay vì Packet/Port
};
```
Đơn giản hơn nhiều so với route qua `Packet`, và về mặt kiến trúc là đúng (đây luôn là kết nối on-tile ngắn, không cần mô phỏng độ trễ dây dẫn/trọng tài NoC).

### 3.10 MMU — 2 tầng, tái dùng `arch::riscv::Walker` có điều kiện

📖 `pagetable_walker.hh:64,199`: `Walker` là `ClockedObject` có sẵn `WalkerPort : RequestPort`, hỗ trợ nhiều outstanding walk — **tái dùng được trực tiếp cho phần mạch/timing**. Nhưng `Walker::start(ThreadContext* _tc, ...)` bắt buộc đọc `satp` từ 1 `ThreadContext` thật.

🔧 **Giải pháp**: viết 1 lớp `ShadowThreadContext : public ThreadContext` tối giản, override duy nhất hàm đọc misc-reg liên quan tới `satp`/privilege để trả về giá trị `shadow_ptbr`/`shadow_status` mà `AuroraManager` đang giữ (cập nhật qua `mUStatus`/`mUPtbr`), các hàm khác throw `panic()` (không bao giờ được gọi tới trong ngữ cảnh PTW-only). Cách này tái dùng 100% logic PTW thật của gem5 (bao gồm cả PMA/PMP check nếu có), chỉ thay nguồn cấp `satp`.

Tầng 1 (Gemmini FrontendTLB): **cần tự viết** — 1 cache nhỏ (4 entry mặc định, `use_shared_tlb=true` nghĩa là 1 mảng dùng chung cho cả Load và Store) ánh xạ VA→PA, miss thì forward request lên tầng 2 (Manager's Walker instance qua con trỏ trực tiếp, theo §3.9).

### 3.11 Danh sách case biên PHẢI implement đúng (đã pass-check với `test.c` chuẩn)

| # | Case | Hành vi RTL chính xác |
|---|---|---|
| 1 | Acquire khi Manager đang IDLE | Grant, `state:=s_active`, set shadow ptbr/status pending |
| 2 | Acquire khi Manager đang ACQUIRED bởi client khác | Deny (`data(0)=0`), client tự thử accelerator khác trong pool |
| 3 | Acquire khi `mgr_id` không hợp lệ | No-op cục bộ tức thời, KHÔNG gửi lên NoC |
| 4 | Acquire khi `cfg_acq_state != s_idle` | Lệnh CSR ghi bị **STALL Ở PIPELINE CPU** (không phải lỗi/no-op) |
| 5 | Acquire khi cfg ĐÃ acquired (double-acquire) | **No-op tuyệt đối**, không gửi gói tin, không đổi state |
| 6 | Release khi cfg CHƯA acquire | No-op với `acq`, nhưng **field `.mgr` vẫn bị ghi đè** |
| 7 | Release → SFENCE | Chỉ hoàn tất sau khi accelerator drain hết (`!busy && inst_q==0`) |
| 8 | Instruction issue khi hết credit | Client trả `io.cmd.ready=false`, CPU thấy RoCC busy |
| 9 | Writeback | CHỈ xảy ra nếu accelerator tự assert `io.resp.valid` (Gemmini: chỉ `COUNTER_OP`) |
| 10 | Fence | Dựa vào `io.rocc.busy` (rocket-chip `RocketCore.scala:412-417`), KHÔNG dựa vào Pending-Count cục bộ |

⚠️ **Case KHÔNG có trong bộ test chuẩn, cần bạn tự viết testcase riêng**: lệnh RoCC với `xs1=0, xs2=1` (nghi vấn bug tại `Client.scala:50-52`, logic route beat `rs2` có thể bị bỏ sót). Nếu implement gem5 model theo đúng RTL "as-is" (để giữ khả năng so sánh 1-1 với silicon), PHẢI port nguyên bug này và ghi chú rõ trong code (`// NOTE: intentionally replicates suspected RTL bug at Client.scala:50-52, verify with RTL testbench before "fixing"`).

---

## 4. User Guide — Firmware API

> Tài liệu này bám theo đúng chuẩn `rerocc/software/rerocc.h` — nếu bạn chọn phương án CSR thật (§3.2), API dưới đây chạy được nguyên bản trên gem5 model.

### 4.1 Header cần include

```c
#include "rerocc.h"   // API acquire/release/assign/fence
#include "rocc.h"     // Macro phát lệnh RoCC custom (ROCC_INSTRUCTION_*)
```

### 4.2 Quy trình sử dụng chuẩn (4 bước)

```c
// Bước 1: ACQUIRE — xin quyền dùng 1 accelerator vật lý (accelId) vào 1 "slot" (cfgId, 0-15)
uint32_t cfgId = 0;
uint32_t accelId = 2;      // ID accelerator vật lý muốn dùng
bool ok = rr_acquire_single(cfgId, accelId);
if (!ok) {
    // Bị từ chối (Manager đang bận với client khác) — thử accelerator khác trong pool:
    uint32_t pool[] = {0, 1, 2, 3};
    ok = rr_acquire_multi(cfgId, pool, 4);
}

// Bước 2: ASSIGN — map cfgId vào 1 trong 4 opcode custom khả dụng
uint32_t opc = 0;           // dùng custom0
rr_set_opc(opc, cfgId);

// Bước 3: ISSUE — phát lệnh gia tốc bình thường qua opcode đã map
//   (ví dụ dưới dùng accelerator Accumulator mẫu trong test.c gốc)
ROCC_INSTRUCTION_SS(opc, value, addr, funct_write);

// Bước 4: FENCE + RELEASE khi xong việc
rr_fence(cfgId);            // đảm bảo accelerator đã xử lý xong MỌI lệnh trước đó
rr_release(cfgId);          // trả accelerator về pool chung
```

### 4.3 Bảng API đầy đủ

| Hàm | Ý nghĩa | Blocking? |
|---|---|---|
| `rr_acquire_single(cfgId, accelId)` | Xin acquire 1 accelerator cụ thể | **CÓ** — CPU stall tới khi có kết quả (grant/deny) |
| `rr_acquire_multi(cfgId, accelIds[], n)` | Thử lần lượt n accelerator trong pool | Mỗi lần thử đều blocking |
| `rr_set_opc(opc, cfgId)` | Map opcode custom → cfgId | Không (CSR local thuần) |
| `rr_fence(cfgId)` | Chờ accelerator xử lý xong hết lệnh đang chờ | **CÓ** — chờ tới khi accelerator hết busy |
| `rr_release(cfgId)` | Trả accelerator, kích hoạt SFENCE dọn TLB tầng 2 | Không (CSR value update ngay, nhưng lệnh CSR *tiếp theo* nào chạm rerocc có thể bị stall) |
| `ROCC_INSTRUCTION_*` | Phát lệnh gia tốc thật | Không (miễn còn credit) |

### 4.4 Ví dụ đầy đủ — tương đương `test.c` gốc, dùng cho Gemmini

```c
#include "rerocc.h"
#include "rocc.h"
#include "gemmini.h"     // header ISA của Gemmini (mvin/mvout/compute/...)

int main() {
    const uint32_t POOL_SIZE = 4;
    uint32_t pool[POOL_SIZE] = {0, 1, 2, 3};   // 4 Gemmini tile khả dụng

    for (uint32_t cfg = 0; cfg < POOL_SIZE; cfg++) {
        if (!rr_acquire_multi(cfg, pool, POOL_SIZE)) {
            printf("cfg %u: acquire that bai, het accelerator kha dung\n", cfg);
            continue;
        }
        rr_set_opc(cfg /* dung opcode = cfg, 0..3 */, cfg);
    }

    // --- Phat lenh Gemmini qua opcode 0 (anh xa toi cfg 0) ---
    gemmini_config_ld(0, stride, ...);      // ROCC_INSTRUCTION qua opcode 0
    gemmini_mvin(0, src_addr, spad_addr);
    gemmini_preload(0, ...);
    gemmini_compute(0, ...);
    gemmini_mvout(0, spad_addr, dst_addr);  // ket qua CHUA THAY DUOC boi CPU luc nay!

    // --- BAT BUOC fence truoc khi doc ket qua tu dst_addr ---
    rr_fence(0);
    // Tu day, doc dst_addr bang lenh load binh thuong la AN TOAN
    int32_t result = *(volatile int32_t*)dst_addr;

    for (uint32_t cfg = 0; cfg < POOL_SIZE; cfg++) {
        rr_release(cfg);   // an toan ngay ca voi cfg chua tung acquire thanh cong
    }
    return 0;
}
```

⚠️ **Lỗi thường gặp nhất khi viết firmware mới**: quên `rr_fence()` trước khi đọc kết quả `mvout`. Vì Gemmini **không có cơ chế writeback rd** cho các lệnh tính toán (chỉ `COUNTER_OP` mới có), CPU **không có cách nào khác** để biết dữ liệu đã ghi xong ngoài `rr_fence()`.

---

## 5. Sequence Diagram — diễn giải chi tiết từng bước

> File tham chiếu: `aurora_sq.puml` (Client↔Manager, 9 mục `==...==`, đánh số 0-8) và `gemmini_sq.puml` (Manager↔Gemmini, 10 mục `==...==`, đánh số 0-9). Phần này diễn giải **từng bước một, không gộp tắt**, theo đúng thứ tự xuất hiện trong file `.puml` — kỹ sư có thể đọc song song 2 file để đối chiếu trực quan.

### 5.0 Cách đọc nhanh 2 file sequence diagram (cho người chưa quen PlantUML)

- Mỗi `participant` trong file `.puml` = 1 "vai" trong câu chuyện (có thể là 1 SimObject thật, hoặc 1 sub-block bên trong SimObject).
- Mũi tên `A -> B : message` = A gọi/gửi gì đó cho B. Mũi tên `-->` (nét đứt) = đường trả lời (response), phân biệt với `->` (nét liền) = yêu cầu (request).
- Khối `activate`/`deactivate` = khoảng thời gian participant đó "đang xử lý" — trong code C++, tương ứng khoảng thời gian 1 hàm/1 state đang active, hữu ích để biết nên đặt state ở đâu.
- Khối `alt/else/end` = rẽ nhánh điều kiện — **mỗi nhánh đều phải có code xử lý riêng**, đây thường là nơi dễ bỏ sót case nhất khi implement.
- Khối `note over X` = chú thích, thường chứa trích dẫn RTL chính xác (`file.scala:line`) — coi đây là "unit test spec" cho hàm tương ứng.
- Số thứ tự bên trái mỗi dòng (autonumber) = thứ tự thời gian thực tế, dùng để đối chiếu khi debug trace log trong gem5 (nên in log kèm số bước tương ứng).

Bảng tra nhanh: bước nào trong `.puml` ứng với hàm nào trong tài liệu này:

| Bước `.puml` | Tên trong `aurora_sq.puml` | Mục trong tài liệu |
|---|---|---|
| 0 | Định nghĩa opcode/CSR | §5.1.0 |
| 1 | `rerocc_acquire` | §5.1.1 |
| 2 | Auto-resync Shadow Regs | §5.1.2 |
| 3 | `rerocc_assign` | §5.1.3 |
| 4 | Issue lệnh RoCC | §5.1.4 |
| 5 | Execution & Address Translation | §5.1.5 (nối sang §5.2) |
| 6 | Writeback có điều kiện | §5.1.6 |
| 7 | `rerocc_fence` | §5.1.7 |
| 8 | `rerocc_release` | §5.1.8 |

| Bước `.puml` | Tên trong `gemmini_sq.puml` | Mục trong tài liệu |
|---|---|---|
| 0 | Ghi chú kiến trúc tổng quan | §5.2.0 |
| 1 | Manager forward RoCCCommand | §5.2.1 |
| 2 | Reservation Station: alloc + dependency | §5.2.2 |
| 3 | Issue tới 3 Controller | §5.2.3 |
| 4 | Load path (DMA đọc) | §5.2.4 |
| 5 | Execute path (Mesh) | §5.2.5 |
| 6 | Store path (DMA ghi) | §5.2.6 |
| 7 | Xoá dependency | §5.2.7 |
| 8 | FLUSH_CMD | §5.2.8 |
| 9 | Tổng hợp `io.busy` | §5.2.9 |

### 5.1 `aurora_sq.puml` — Giao thức Client ↔ Manager

> **Bức tranh lớn trước khi vào chi tiết**: toàn bộ 9 bước dưới đây giải quyết đúng 1 bài toán duy nhất — **làm sao để nhiều CPU dùng chung 1 pool accelerator một cách an toàn, hiệu quả, mà mỗi CPU vẫn "cảm giác" như đang dùng riêng 1 accelerator gắn cứng cho mình** (đúng trải nghiệm RoCC truyền thống). Muốn vậy hệ thống phải giải 4 bài toán con: (1) ai đang được dùng accelerator nào — **acquire/release**; (2) accelerator dịch địa chỉ đúng theo ngữ cảnh của CHỦ SỞ HỮU hiện tại — **shadow register sync**; (3) không gian mã lệnh RISC-V có hạn (chỉ 4 opcode custom) nhưng số accelerator có thể acquire nhiều hơn — **assign/opcode mapping**; (4) CPU biết khi nào an toàn đọc kết quả — **fence**. Mỗi bước dưới đây map trực tiếp vào 1 trong 4 bài toán con này.

#### 5.1.0 — Định nghĩa hằng số dùng chung

**Mục đích thiết kế**: đây không phải 1 bước "chức năng" mà là bước chuẩn bị bắt buộc — trước khi 2 SimObject (`AuroraClient`, `AuroraManager`) có thể "nói chuyện" với nhau, chúng cần thống nhất **ý nghĩa từng bit** trong gói tin trao đổi. Đây chính là vai trò của 1 giao thức (protocol): quy ước để 2 bên độc lập hiểu đúng nhau mà không cần biết chi tiết implement bên kia.

**Logic hoạt động**: gói tin ReRoCC có 4 trường cố định (`opcode` 3-bit, `client_id`, `manager_id`, `data` 64-bit) và **opcode dùng LẠI cùng giá trị số cho 2 chiều khác nhau** (chiều Client→Manager và chiều Manager→Client) — đây là thiết kế tiết kiệm bit hợp lý ở RTL (vì 2 chiều đi trên 2 "wire" vật lý riêng, không bao giờ nhầm lẫn về mặt điện), nhưng khi bạn mô hình hoá trong C++ (nơi không có "wire riêng" mà chỉ có 1 hàm callback chung), nếu không tách rõ 2 enum, code rất dễ nhầm `sAcqResp` (giá trị 0) với `mAcquire` (cũng giá trị 0) khi debug log chỉ in ra con số.

**Vai trò của từng model tại bước này**: chưa có model nào "chạy" — đây thuần là hợp đồng (contract) giữa `AuroraClient.cc` và `AuroraManager.cc`, nên đặt trong 1 header dùng chung để đảm bảo 2 file luôn đồng bộ khi 1 trong 2 team sửa đổi.

```cpp
// aurora_protocol.hh
enum class ReRoCCOpcode : uint8_t {
    mAcquire=0, mInst=1, mUStatus=2, mUPtbr=3, mRelease=4, mUnbusy=5,
    sAcqResp=0, sInstAck=1, sWrite=2, sRelResp=3, sUnbusyAck=4
};
constexpr int NUM_CFG = 16;
constexpr int NUM_OPC_SLOTS = 4;
constexpr int CREDIT_DEPTH = 4;
```

🎯 **Điểm mấu chốt**: tách 2 `enum class` riêng (`ReRoCCReqOpcode`/`ReRoCCRespOpcode`) thay vì dùng chung 1 enum như trên — tận dụng type-safety của C++ để compiler tự bắt lỗi nếu code lỡ gửi nhầm 1 opcode-chiều-đi vào hàm xử-lý-chiều-về.

---

#### 5.1.1 — `rerocc_acquire`: CPU ghi CSR `RRCFGn`

**Mục đích thiết kế**: đây là bước giải quyết bài toán "**ai đang sở hữu accelerator nào**" — nói cách khác, đây chính là cơ chế **lock** trong 1 hệ thống multi-tenant, nhưng là lock ở tầng SoC (giữa nhiều Client silicon) chứ không phải lock phần mềm thông thường. Cái khó ở đây là: quyết định "đồng ý hay từ chối" nằm ở **Manager** (bên nhận yêu cầu), nhưng **CPU** (bên gửi yêu cầu) lại cần biết kết quả *trước khi* tiếp tục thực thi lệnh kế tiếp — vì lệnh kế tiếp rất có thể là "phát lệnh tính toán cho accelerator vừa acquire", mà nếu acquire thất bại thì lệnh đó vô nghĩa. Đây chính là lý do bước này **bắt buộc phải blocking** (CPU đứng chờ), khác hẳn triết lý "non-blocking" của các RoCC accelerator truyền thống.

**Logic hoạt động chi tiết** — đây là bước có nhiều nhánh rẽ nhất trong toàn bộ giao thức, vì phải xử lý đủ MỌI trạng thái CSR có thể gặp (không được để trạng thái nào "không xác định"):

1. Trước tiên kiểm tra **tính hợp lệ tĩnh** của yêu cầu: `mgr_id` được ghi vào CSR có nằm trong danh sách Manager mà Client này thực sự kết nối tới hay không (thông tin này biết được ngay tại thời điểm build hệ thống, không cần hỏi ai). Nếu không hợp lệ — đây là lỗi lập trình rõ ràng (địa chỉ Manager không tồn tại), nên xử lý cục bộ ngay, không đáng phải trả giá 1 round-trip NoC.
2. Tiếp theo kiểm tra **tài nguyên điều phối nội bộ có đang rảnh không** (`fsmState`). Đây là điểm rất dễ bị hiểu lầm: Client chỉ có **1 bộ máy điều phối duy nhất cho TẤT CẢ 16 accelerator có thể acquire**, không phải 16 bộ máy độc lập. Lý do thiết kế thế này (thay vì 16 FSM song song) là **tiết kiệm phần cứng** — vì acquire/release là thao tác **hiếm** (so với phát lệnh tính toán, vốn xảy ra liên tục), nên không đáng để nhân bản 16 lần logic phức tạp cho 1 việc ít khi xảy ra. Cái giá phải trả: nếu đúng lúc đang acquire accelerator A thì không thể ĐỒNG THỜI acquire/release accelerator B — phải xếp hàng. Đây không phải bug, là đánh đổi thiết kế có chủ đích.
3. Nếu qua được 2 kiểm tra trên, xét tiếp **trạng thái hiện tại của chính "ngăn" (cfg slot) đang muốn thao tác**: đang acquire mà acquire tiếp = vô nghĩa (không làm gì); đang rảnh mà release = vô nghĩa (không làm gì, dù vẫn cho phép cập nhật vài trường phụ để không "lãng phí" thao tác ghi CSR của phần mềm); chỉ 2 trường hợp còn lại (rảnh→acquire, đang giữ→release) mới thật sự cần "nói chuyện" với Manager qua NoC.
4. Với 2 trường hợp thật sự cần giao tiếp: **acquire** phải blocking-chờ-kết-quả (vì như giải thích ở trên, CPU cần biết ngay); **release** lại **không cần blocking** — vì về bản chất, "trả lại tài nguyên" không có gì để CPU phải chờ xác nhận trước khi làm việc khác (khác acquire, nơi "được cấp hay không" ảnh hưởng trực tiếp lệnh tiếp theo). Đây là 1 sự bất đối xứng có chủ đích trong thiết kế giao thức, không phải ngẫu nhiên.

**Vai trò của từng model tại bước này**:
- **CPU**: chỉ đóng vai "người gọi", không có logic gì đặc biệt ngoài việc chấp nhận bị đứng hình khi cần.
- **AuroraClient**: đóng vai "người gác cổng" — quyết định request có đáng gửi lên NoC hay không TRƯỚC KHI gửi, để tránh lãng phí băng thông NoC cho những yêu cầu chắc chắn sẽ vô nghĩa hoặc sai.
- **AuroraManager**: đóng vai "trọng tài" — chỉ có 1 quyết định duy nhất phải đưa ra (đang rảnh hay không), cực kỳ đơn giản về logic nhưng lại là điểm quyết định toàn bộ tính đúng đắn của cơ chế multi-tenant (nếu trọng tài xử lý sai, 2 tenant có thể cùng nghĩ mình đang sở hữu 1 accelerator — lỗi nghiêm trọng).

```cpp
CSRWriteResult AuroraClient::onWriteRRCFG(int cfgId, uint16_t wdata) {
    bool wantAcq = (wdata >> 8) & 1;
    uint8_t mgrId = wdata & 0xFF;
    bool oldAcq = cfgTable[cfgId].acq;

    if (wantAcq && !validMgr(mgrId)) return CSRWriteResult::COMPLETED_NOOP;
    if (fsmState != FsmState::IDLE) return CSRWriteResult::MUST_STALL;
    if (wantAcq && oldAcq) return CSRWriteResult::COMPLETED_NOOP;
    if (!wantAcq && !oldAcq) { cfgTable[cfgId].mgr = mgrId; return CSRWriteResult::COMPLETED_NOOP; }

    if (wantAcq && !oldAcq) {
        fsmState = FsmState::ACQ;
        pendingCfgId = cfgId; pendingMgrId = mgrId;
        sendMessage(ReRoCCOpcode::mAcquire, cfgId, mgrId, 0);
        return CSRWriteResult::MUST_STALL;
    }
    fsmState = FsmState::REL;
    pendingCfgId = cfgId; pendingMgrId = cfgTable[cfgId].mgr;
    cfgTable[cfgId] = {.acq=0, .mgr=mgrId};
    sendMessage(ReRoCCOpcode::mRelease, cfgId, pendingMgrId, 0);
    return CSRWriteResult::COMPLETED_NOOP;
}

void AuroraManager::onReceiveMAcquire(int clientId, int mgrId, int cfgId) {
    bool granted = (fsmState == FsmState::IDLE);
    if (granted) { fsmState = FsmState::ACTIVE; currentClientId = clientId; }
    sendMessage(ReRoCCOpcode::sAcqResp, clientId, mgrId, granted ? 1 : 0);
}
```

🎯 **Điểm mấu chốt**: hàm `onWriteRRCFG` trả về **4 loại kết quả ngữ nghĩa khác nhau** dù chỉ có 2 kiểu return type — `MUST_STALL` do "bận nội bộ" (case 2, chỉ cần đợi rồi thử lại đúng lệnh này, không cần logic gì thêm) về bản chất **hoàn toàn khác** `MUST_STALL` do "đang chờ NoC trả lời" (case acquire thật, cần Client chủ động "đánh thức" CPU khi có `sAcqResp` về). Nếu lớp ISA phía trên gộp 2 loại `MUST_STALL` này làm một, rất dễ code sai cơ chế "ai đánh thức CPU dậy" và gây deadlock giả (CPU chờ mãi không ai đánh thức).

---

#### 5.1.2 — Tự động đồng bộ Shadow Registers

**Mục đích thiết kế**: đây là bước giải quyết bài toán con thứ 2 — accelerator (vật lý, dùng chung) phải biết **đang chạy hộ ai** để dịch địa chỉ ảo→vật lý đúng ngữ cảnh (mỗi tiến trình có 1 page table riêng). Vì Manager không "sống" trong CPU nên không thể tự đọc `mstatus`/`satp` của CPU — phải được CPU **chủ động gửi bản sao (shadow copy)** qua NoC. Điểm thiết kế thông minh ở đây: bước này được kích hoạt **tự động ngay sau acquire thành công**, phần mềm không cần gọi hàm nào riêng — giảm gánh nặng cho người viết firmware và tránh lỗi "quên đồng bộ context" (1 lớp lỗi rất phổ biến trong các hệ thống multi-context thủ công).

**Logic hoạt động chi tiết**: `mstatus` là thanh ghi 128-bit trong khi mỗi gói tin ReRoCC chỉ mang 64-bit data → bắt buộc phải **chia làm 2 gói tin liên tiếp** (`mUStatus` beat0 = nửa thấp, beat1 = nửa cao). `ptbr` (page table base register) chỉ 64-bit nên gửi gọn trong 1 gói. Cả 3 gói tin này (2× status + 1× ptbr) đi **qua cùng 1 FSM điều khiển dùng chung** như bước acquire — nghĩa là về bản chất, "1 lần acquire" thật ra là **4 giao dịch NoC tuần tự** (acquire + 2 status + 1 ptbr), và CPU **đứng chờ liên tục suốt cả 4 giao dịch này**, không chỉ chờ riêng giao dịch acquire đầu tiên. Đây là điểm rất dễ bị code sai nếu chỉ đọc lướt tên bước "acquire" mà không nhận ra chuỗi phụ trợ phía sau.

**Vai trò của từng model**: **AuroraClient** đóng vai "người chuyển phát tự động" — không cần phần mềm ra lệnh, tự trigger chuỗi 3 gói tin dựa trên 2 cờ nội bộ (`cfg_updatestatus`/`cfg_updateptbr`) được bật ngay khi nhận `sAcqResp` thành công. **AuroraManager** đóng vai "nơi lưu trữ ngữ cảnh" — nhận xong 3 gói tin này mới thật sự có đủ thông tin để dịch địa chỉ đúng cho accelerator ở các bước sau.

```cpp
if (granted) {
    cfgTable[pendingCfgId].acq = 1;
    fsmState = FsmState::STATUS0;   // tự động nối tiếp STATUS1 -> PTBR -> IDLE
} else {
    cfgTable[pendingCfgId].acq = 0;
    fsmState = FsmState::IDLE;
    wakeupStalledCSRInstruction();
}
```

🎯 **Điểm mấu chốt**: chỉ gọi `wakeupStalledCSRInstruction()` khi `fsmState` quay về `IDLE` **thật sự** (tức sau khi NHẬN xong phản hồi cho cả `mUPtbr` cuối cùng), không phải ngay khi nhận `sAcqResp` của riêng bước acquire. Đánh thức CPU quá sớm sẽ khiến phần mềm tưởng acquire đã "xong toàn bộ" trong khi shadow context chưa đồng bộ đủ, dẫn tới lệnh tính toán đầu tiên dùng nhầm `ptbr` cũ.

---

#### 5.1.3 — `rerocc_assign` (ghi CSR `RROPCn`)

**Mục đích thiết kế**: đây là lời giải cho bài toán con thứ 3 — RISC-V chỉ dành đúng 4 mã lệnh "custom" (`custom0-3`) cho mở rộng phần cứng, nhưng hệ thống lại cho phép 1 CPU acquire **tới 16 accelerator cùng lúc**. Bước `assign` chính là lớp gián tiếp (indirection) giải quyết mâu thuẫn này: thay vì cố nhồi 16 khả năng vào 4 bit mã lệnh (không đủ chỗ), hệ thống tách "quyền sở hữu accelerator" (16 slot) ra khỏi "mã lệnh CPU phát ra" (4 slot), rồi dùng 1 bảng ánh xạ nhỏ ở giữa. Đây là 1 pattern kiến trúc rất phổ biến (giống bảng trang bộ nhớ ánh xạ không gian ảo lớn vào không gian vật lý nhỏ hơn) — chỉ khác là áp dụng cho không gian mã lệnh thay vì không gian địa chỉ.

**Logic hoạt động**: đây là thao tác **thuần cục bộ** — không có gì để "thoả thuận" với Manager, vì việc CPU này chọn dùng mã lệnh nào để gọi accelerator nào là quyết định hoàn toàn nội bộ của Client, Manager không cần biết và không quan tâm. Vì vậy bước này hoàn tất trong đúng 1 chu kỳ, không round-trip NoC, không blocking CPU.

**Vai trò của từng model**: chỉ **AuroraClient** hoạt động ở bước này — ghi giá trị vào 1 bảng tra cứu nhỏ (4 entry) mà chính nó sẽ dùng lại ngay ở bước 5.1.4 kế tiếp.

```cpp
void AuroraClient::onWriteRROPC(int opcSlot, int cfgId) {
    opcTable[opcSlot] = cfgId;
}
```

🎯 **Điểm mấu chốt**: chính vì bước này không có xác nhận từ Manager, nên **không có gì ngăn phần mềm ghi 1 `cfgId` chưa từng acquire** vào `RROPCn` — hệ thống không báo lỗi ngay tại đây; lỗi (nếu có) chỉ lộ ra muộn hơn, ở bước 5.1.4 khi thật sự phát lệnh (vì lúc đó mới kiểm tra `cfgTable[cfgId].acq`). Khi debug, nếu thấy lệnh tính toán bị "treo mãi không gửi đi", hãy kiểm tra lại đúng 2 bước 5.1.1 và 5.1.3 có thật sự khớp `cfgId` hay không trước khi nghi ngờ logic phức tạp hơn.

---

#### 5.1.4 — Issue lệnh RoCC (data-plane)

**Mục đích thiết kế**: đây là bước "làm việc thật sự" — mọi bước trước đó (acquire, sync context, assign) chỉ là chuẩn bị; đây mới là nơi giá trị tính toán thật sự được tạo ra. Vì bước này xảy ra **liên tục, tần suất cao** (khác hẳn acquire/release vốn hiếm), triết lý thiết kế ở đây đảo ngược hoàn toàn so với bước 5.1.1: **non-blocking bằng mọi giá**. Nếu mỗi lệnh tính toán đều phải CPU đứng chờ xác nhận round-trip NoC như acquire, hiệu năng hệ thống sẽ tệ không thể chấp nhận (tưởng tượng: 1 vòng lặp phát hàng nghìn lệnh `mvin`/`compute`, mỗi lệnh chờ NoC round-trip vài chục ns → tổng thời gian tính riêng phần "chờ mạng" đã lớn hơn cả thời gian tính toán thật).

Để đạt non-blocking mà vẫn không làm tràn ngập Manager (vốn có bộ đệm hữu hạn), hệ thống dùng **credit-based flow control** — 1 kỹ thuật kinh điển trong thiết kế NoC/pipeline: bên gửi chỉ được gửi khi còn "tín dụng" (credit), tín dụng giảm khi gửi, tăng khi bên nhận xác nhận đã có chỗ xử lý tiếp. Đây là cách đạt non-blocking (không cần chờ round-trip cho MỖI lệnh) mà vẫn đảm bảo không có lệnh nào bị Manager âm thầm huỷ vì hết chỗ.

**Logic hoạt động chi tiết**: trước khi cho phép 1 lệnh rời khỏi Client, phải xác nhận **4 điều kiện độc lập** đều đúng cùng lúc, mỗi điều kiện bảo vệ 1 loại tài nguyên khác nhau:
- Còn credit **và** cfg đang thật sự acquired — bảo vệ bộ đệm phía Manager không bị tràn, đồng thời chặn việc lỡ gửi lệnh cho 1 accelerator đã bị release.
- Không có cập nhật status/ptbr nào đang chờ gửi — bảo vệ tính đúng đắn: nếu shadow context CHƯA đồng bộ xong (bước 5.1.2 chưa hoàn tất hoặc vừa có thay đổi mới như context switch), gửi lệnh lúc này accelerator sẽ dịch địa chỉ SAI (dùng ptbr cũ).
- FSM điều khiển đang rảnh — đây chính là hệ quả của quyết định thiết kế "1 FSM dùng chung 16 cfg" đã nói ở bước 5.1.1: nếu ĐANG có 1 giao dịch acquire/release/sync khác (dù cho 1 cfg hoàn toàn khác) diễn ra, lệnh tính toán của CFG NÀY cũng bị chặn theo — đây là cái giá thật sự phải trả cho việc tiết kiệm phần cứng ở bước 5.1.1, và là điều quan trọng nhất cần mô phỏng đúng nếu bạn muốn số liệu hiệu năng multi-tenant chính xác.

Chỉ khi cả 4 điều kiện đều thoả, lệnh mới thật sự được đóng gói gửi đi — và **ngay lập tức trừ 1 credit**, không đợi xác nhận (vì đợi xác nhận mới trừ sẽ tạo ra khoảng hở race-condition nếu 2 lệnh liên tiếp cùng kiểm tra credit trước khi lệnh đầu kịp trừ).

**Vai trò của từng model**: **AuroraClient** đóng vai "người điều tiết lưu lượng" (rate limiter) — công việc duy nhất là bảo vệ Manager khỏi bị quá tải trong khi vẫn tối đa hoá thông lượng. **AuroraManager** (sẽ thấy rõ ở bước tiếp) đóng vai "người xác nhận tiến độ thật" — không phải khi NHẬN được lệnh mà khi THẬT SỰ chuyển giao được cho accelerator mới coi là "đã xử lý xong phần việc của Manager", credit mới được hoàn trả.

```cpp
bool AuroraClient::tryIssueInstruction(RoCCCmd cmd) {
    int opcSlot = (cmd.opcode >> 5) & 0b11;
    int cfgId = opcTable[opcSlot];

    bool creditOk = credits[cfgId] > 0 && cfgTable[cfgId].acq;
    bool statusOk = !pendingStatusUpdate[cfgId];
    bool ptbrOk   = !pendingPtbrUpdate[cfgId];
    bool fsmOk    = (fsmState == FsmState::IDLE);

    if (!(creditOk && statusOk && ptbrOk && fsmOk)) return false;

    credits[cfgId]--;
    sendMultiBeatMessage(ReRoCCOpcode::mInst, cfgId, cmd);
    return true;
}
```

⚠️ **Nhắc lại lỗi hay gặp nhất toàn tài liệu**: `sInstAck` (trả credit) fire ở Manager tại đúng thời điểm lệnh được **dequeue từ `inst_q` sang `io.cmd` của Gemmini** — KHÔNG PHẢI khi Manager mới nhận/enqueue. Lý do logic đằng sau: credit phải phản ánh đúng **khả năng thật của TOÀN BỘ đường ống phía sau**, không chỉ riêng chỗ đệm của Manager — nếu trả credit quá sớm (ngay khi enqueue), Client sẽ nghĩ "đường ống rảnh" trong khi Gemmini phía sau vẫn đang tắc, dẫn tới đo sai độ trễ thật khi mô phỏng. Xem code mẫu đầy đủ ở §3.8.

---

#### 5.1.5 — Execution & Address Translation (phía Manager)

**Mục đích thiết kế**: đây là điểm bàn giao — Manager không tự thực thi tính toán (đó là việc của Gemmini), vai trò của Manager tại đây thuần là **forward đúng, đúng thời điểm**. Bước này về bản chất chỉ là 1 câu lệnh gọi hàm đơn giản, nhưng ý nghĩa kiến trúc của nó là ranh giới rõ ràng: **mọi thứ TRƯỚC bước này (5.1.1-5.1.4) thuộc về "giao thức AuRORA"** (chung cho MỌI accelerator, không riêng gì Gemmini), còn **mọi thứ SAU bước này thuộc về "logic riêng của Gemmini"** (§5.2). Ranh giới này rất quan trọng khi thiết kế code: `AuroraManager` không nên chứa bất kỳ logic nào biết về Reservation Station/Scratchpad/Mesh của Gemmini — nếu sau này bạn thay Gemmini bằng 1 accelerator khác, chỉ cần thay đổi phía sau ranh giới này.

Chi tiết đầy đủ về logic phía sau ranh giới này (những gì xảy ra bên trong Gemmini) được trình bày trọn vẹn ở §5.2.

```cpp
// Điểm nối — xem chi tiết đầy đủ tại §5.2.1
gemmini->pushCmd(cmd);
```

---

#### 5.1.6 — Writeback (CÓ ĐIỀU KIỆN)

**Mục đích thiết kế**: về nguyên tắc, RoCC là 1 giao diện "coprocessor" chuẩn — CPU phát lệnh kèm toán hạng, accelerator TÙY CHỌN trả về 1 giá trị ghi vào thanh ghi đích (`rd`). Chữ "tuỳ chọn" ở đây là mấu chốt: giao thức ReRoCC được thiết kế **trung lập với mọi loại accelerator**, nên bước writeback phải **phản ánh trung thực quyết định của chính accelerator** (có trả giá trị hay không), Manager không được tự ý "giả lập" 1 giá trị trả về nếu accelerator không cung cấp. Đây là lý do bước này PHẢI có điều kiện, không phải 1 lỗi thiết kế.

**Logic hoạt động**: Manager chỉ đơn thuần **quan sát** tín hiệu accelerator có "muốn nói gì" hay không (`io.resp.valid`) — nếu có, đóng gói thành 2 gói tin (`data` trước, `rd` sau, vì băng thông mỗi gói giới hạn 64-bit trong khi cần truyền 2 giá trị) và gửi trả Client; nếu không, không làm gì cả — im lặng hoàn toàn, không có bất kỳ gói tin "báo không có gì để trả" nào được gửi đi (tiết kiệm băng thông NoC cho trường hợp phổ biến — đa số lệnh tính toán KHÔNG trả giá trị qua đường này, như sẽ thấy rõ ở §5.2).

**Vai trò của từng model**: **Gemmini** (hoặc accelerator bất kỳ được gắn) là bên duy nhất quyết định có "nói" hay không. **AuroraManager** chỉ là người chuyển tiếp trung thực, không thêm bớt logic nghiệp vụ.

```cpp
void AuroraManager::onGemminiResp(bool valid, uint64_t data, uint8_t rd) {
    if (!valid) return;
    sendMessage(ReRoCCOpcode::sWrite, currentClientId, data);
    sendMessage(ReRoCCOpcode::sWrite, currentClientId, rd);
}
```

🎯 **Điểm mấu chốt**: với Gemmini cụ thể, hàm này gần như không bao giờ được gọi với `valid=true` (chỉ đúng 1 loại lệnh — xem §5.2.9 để hiểu tại sao). Đừng lấy "RoCC writeback" làm cơ chế chính để biết accelerator đã tính xong — đó là vai trò của bước fence (5.1.7), không phải writeback.

---

#### 5.1.7 — `rerocc_fence`

**Mục đích thiết kế**: đây là lời giải cho bài toán con thứ 4 — "CPU biết khi nào an toàn đọc kết quả". Vấn đề gốc rễ mà bước này giải quyết: accelerator (Gemmini) hoạt động **hoàn toàn bất đồng bộ** với CPU (CPU phát lệnh xong là tiếp tục ngay, không đợi accelerator tính xong — đúng tinh thần non-blocking của bước 5.1.4), nhưng CPU vẫn cần 1 điểm đồng bộ hoá tường minh trước khi tin tưởng đọc dữ liệu mà accelerator ghi ra bộ nhớ — nếu không, sẽ có race-condition đọc phải dữ liệu chưa kịp ghi xong. `fence` chính là điểm đồng bộ hoá đó — nhưng nó KHÔNG dùng cơ chế giống acquire (không phải "chờ 1 gói tin trả lời rồi xong"), mà là "chờ TỚI KHI accelerator thật sự rảnh hoàn toàn" — đây là 1 dạng "barrier" (rào chắn đồng bộ), khác bản chất với "request-response" của acquire.

**Logic hoạt động chi tiết**: có 2 lớp bảo vệ lồng nhau đáng chú ý. Lớp thứ nhất, phía Client: trước khi thật sự gửi tín hiệu "hỏi Manager có rảnh chưa" (`mUnbusy`), Client phải tự đảm bảo **không có lệnh nào của chính nó đang dang dở trên đường truyền** — nếu gửi `mUnbusy` trong khi vẫn còn 1 lệnh `mInst` chưa kịp tới Manager, câu trả lời "rảnh" nhận được có thể sai (Manager trả lời dựa trên trạng thái CHƯA bao gồm lệnh đang bay trên NoC). Lớp thứ hai, phía Manager: chỉ trả lời "đã rảnh" khi **cả hàng đợi lệnh nội bộ rỗng VÀ chính accelerator báo hết bận** — thiếu 1 trong 2 điều kiện đều sai (hàng đợi rỗng nhưng accelerator vẫn đang tính lệnh cuối cùng lấy ra từ hàng đợi = chưa xong; ngược lại accelerator "tạm nghỉ" giữa 2 lệnh nhưng hàng đợi vẫn còn lệnh chờ = cũng chưa xong).

Điểm đặc biệt quan trọng về mặt kiến trúc CPU (đã xác nhận xuyên 2 repo `rerocc`+`rocket-chip`): bản thân lệnh RISC-V chuẩn `fence` **không có nghĩa gì đặc biệt với accelerator theo định nghĩa ISA gốc** (nó chỉ đảm bảo thứ tự truy cập bộ nhớ giữa các lệnh trên chính CPU đó) — sở dĩ `rerocc_fence()` "mượn" được lệnh `fence` để tạo ra rào chắn đúng nghĩa là nhờ **core CPU được thiết kế đặc biệt để kiểm tra tín hiệu "RoCC còn bận" mỗi khi gặp lệnh `fence`**, và tự ý đứng khựng lại nếu bận. Đây là 1 sự phối hợp thiết kế xuyên suốt 2 tầng (ISA-level convention + microarchitecture-level enforcement) — khi mô phỏng trong gem5, bạn phải tái tạo ĐÚNG sự phối hợp 2 tầng này (không thể chỉ code phía AuroraClient mà bỏ qua phần CPU model phải chủ động hỏi AuroraClient trước khi cho lệnh `fence` retire).

**Vai trò của từng model**: **AuroraClient** đóng vai "người khởi động rào chắn" và "người báo cáo trạng thái bận" cho CPU. **AuroraManager** đóng vai "người xác nhận điều kiện rào chắn đã thoả" dựa trên quan sát trực tiếp trạng thái Gemmini. **CPU model** (lớp bạn ít nghĩ tới nhất nhưng lại là mắt xích bắt buộc) đóng vai "người thực thi việc đứng khựng" — không có mắt xích này, toàn bộ 2 vai trên chỉ là trao đổi thông tin vô nghĩa vì chẳng ai thực sự bị chặn lại.

```cpp
void AuroraClient::onWriteRRBAR(int cfgId) {
    if (!cfgTable[cfgId].acq) return;
    fenceState[cfgId] = FenceState::REQ;
    if (instSenderIdle() && !hasPendingCmd()) {
        sendMessage(ReRoCCOpcode::mUnbusy, cfgId, 0);
    }
}

void AuroraManager::onReceiveMUnbusy(int clientId, int cfgId) {
    fsmState = FsmState::UNBUSY;
}
void AuroraManager::checkUnbusyComplete() {   // gọi mỗi tick
    if (fsmState == FsmState::UNBUSY && !gemmini->isBusy() && instQueue.empty()) {
        sendMessage(ReRoCCOpcode::sUnbusyAck, currentClientId, 0);
        fsmState = FsmState::ACTIVE;
    }
}

// Trong CPU model: TRƯỚC khi cho phép lệnh 'fence' retire, phải hỏi:
bool shouldStallForFence() {
    return aurora_client->isRoccBusy();   // bao gồm fenceState != IDLE
}
```

⚠️ **Nhắc lại vì đây từng là "câu hỏi mở" giải quyết được nhờ đọc chéo repo**: nếu bạn chỉ đọc code `rerocc` mà không đọc code CPU core, rất dễ kết luận nhầm rằng `rerocc_fence()` "không có gì đảm bảo" thật sự chặn CPU — kết luận đó SAI, nhưng chỉ sai vì thiếu 1 nửa bức tranh (nửa nằm ở CPU core, không nằm ở `rerocc`). Khi bạn code phần CPU model trong gem5, đừng quên implement chính "nửa còn lại" này.

---

#### 5.1.8 — `rerocc_release`

**Mục đích thiết kế**: đây là bước "trả lại tài nguyên", khép vòng đời acquire→sử dụng→release, để accelerator sẵn sàng phục vụ 1 tenant khác. Điểm khó nhất về mặt thiết kế ở bước này không phải là "trả lại" (đơn giản) mà là: **làm sao đảm bảo tenant TIẾP THEO không nhìn thấy bất kỳ dấu vết nào của tenant TRƯỚC** — đây là bài toán cách ly (isolation), bản chất giống hệt bài toán bảo mật giữa các tiến trình trong 1 hệt điều hành, chỉ khác là áp dụng cho 1 accelerator dùng chung thay vì CPU dùng chung.

**Logic hoạt động chi tiết**: release có 3 giai đoạn nối tiếp, mỗi giai đoạn giải quyết 1 khía cạnh khác nhau của bài toán cách ly:
1. **Giai đoạn "báo nhận ngay"**: giá trị CSR phía Client được cập nhật LẬP TỨC (tenant cũ thấy mình "đã release" ngay), nhưng đây chỉ là kế toán sổ sách phía Client — KHÔNG đồng nghĩa Manager đã thật sự sẵn sàng cho tenant mới. Thiết kế "lạc quan" (optimistic) này giúp CPU không phải đứng chờ vô ích cho 1 thao tác vốn không cần CPU phải biết kết quả ngay (đúng như đã giải thích ở 5.1.1: release không cần blocking).
2. **Giai đoạn "chờ dọn dẹp thật sự"**: Manager không lập tức coi mình "rảnh" ngay khi nhận yêu cầu release — nó phải đợi **mọi công việc còn dang dở của tenant cũ hoàn toàn kết thúc** (giống hệt điều kiện ở fence, và không phải ngẫu nhiên — về bản chất release "bao hàm" 1 fence ngầm bên trong).
3. **Giai đoạn "xoá dấu vết"**: đây là bước **bắt buộc về bảo mật, không phải tối ưu hiệu năng** — sau khi chắc chắn tenant cũ đã hoàn toàn dừng, Manager chủ động xoá sạch bộ nhớ đệm dịch địa chỉ (translation cache) mà nó đang giữ cho tenant cũ, để tenant mới không thể vô tình (hoặc cố ý) hưởng lợi từ 1 bản dịch địa chỉ mà đáng lẽ nó không có quyền biết.

**Vai trò của từng model**: **AuroraClient** đóng vai "người báo cáo ý định", không giữ vai trò gì trong việc đảm bảo cách ly thật sự (đó không phải trách nhiệm của nó). **AuroraManager** đóng **toàn bộ trách nhiệm đảm bảo cách ly** — đây là lý do khi thiết kế test coverage cho gem5 model, phần lớn effort kiểm thử bảo mật nên tập trung vào Manager, không phải Client.

```cpp
void AuroraManager::onReceiveMRelease(int clientId) {
    fsmState = FsmState::REL_WAIT;
}
void AuroraManager::checkReleaseComplete() {   // gọi mỗi tick
    if (fsmState == FsmState::REL_WAIT && !gemmini->isBusy() && instQueue.empty()) {
        sendMessage(ReRoCCOpcode::sRelResp, currentClientId, 0);
        fsmState = FsmState::SFENCE;
        mmuWalker->sfence();     // Giai đoạn 3 — xoá sạch cache dịch địa chỉ
        fsmState = FsmState::IDLE;
    }
}
```

⚠️ **Hệ quả thực tế cần lưu ý khi viết firmware/runtime**: vì giai đoạn 1 và giai đoạn 2-3 KHÔNG đồng bộ với nhau (release "xong" theo Client rất sớm, nhưng Manager có thể còn đang dọn dẹp), nếu phần mềm cố acquire LẠI đúng accelerator vừa release gần như ngay lập tức, có khả năng thật sự bị từ chối tạm thời (Manager chưa kịp về `s_idle`) — runtime/firmware phải có logic thử lại (retry), không được giả định release là tức thời hoàn toàn.

### 5.2 `gemmini_sq.puml` — Giao thức Manager ↔ Gemmini

> **Bức tranh lớn**: nếu §5.1 giải quyết bài toán "ai được dùng accelerator, khi nào an toàn", thì §5.2 giải quyết bài toán hoàn toàn khác — "**làm sao tận dụng tối đa 1 accelerator vật lý cho hiệu năng cao nhất, trong khi vẫn đảm bảo kết quả đúng**". Đây là bài toán kinh điển của kiến trúc máy tính: **song song hoá mà không vi phạm phụ thuộc dữ liệu** (data dependency) — cùng bản chất với 1 CPU out-of-order, chỉ khác quy mô nhỏ hơn và áp dụng riêng cho domain tính toán ma trận.

#### 5.2.0 — Bối cảnh chung, đọc trước khi vào từng bước

**Mục đích của việc đọc trước phần này**: Gemmini không phải "1 hộp đen nhận lệnh, trả kết quả" như nhiều accelerator đơn giản khác — nó có kiến trúc nội bộ phức tạp tương đương 1 lõi xử lý thu nhỏ, với pipeline riêng, bộ nhớ riêng, và cơ chế theo dõi phụ thuộc riêng. Nếu mô hình hoá Gemmini trong gem5 như 1 hàm C++ đơn giản "nhận lệnh → tính → trả kết quả ngay", model sẽ sai hoàn toàn về mặt thời gian (timing) dù có thể vẫn "đúng" về mặt giá trị tính toán — mà mục tiêu chính của việc dùng gem5 là đo **thời gian**, nên đây là sai lầm chí mạng cần tránh.

Có 3 nguyên lý xuyên suốt toàn bộ §5.2 mà mọi bước con đều là hệ quả của chúng:
1. **Nguyên lý "tách rời truy cập khỏi thực thi" (access/execute decoupling)**: việc "lấy dữ liệu vào" (load), "tính toán" (execute), "đẩy dữ liệu ra" (store) được tách thành 3 luồng độc lập, không có luồng nào phải chờ luồng khác nếu không thật sự cần — đây là lý do tồn tại 3 hàng đợi riêng thay vì 1 hàng đợi chung.
2. **Nguyên lý "chỉ chờ khi thật sự phụ thuộc"**: 2 lệnh chỉ phải thực thi theo đúng thứ tự nếu chúng đụng chạm cùng 1 vùng bộ nhớ on-chip; nếu không đụng chạm gì nhau, chúng được tự do thực thi theo bất kỳ thứ tự nào có lợi nhất cho hiệu năng.
3. **Nguyên lý "thông báo tổng hợp, không thông báo chi tiết"**: Gemmini không báo cáo "lệnh nào xong" ra bên ngoài (trừ đúng 1 ngoại lệ) — nó chỉ báo cáo trạng thái tổng hợp "còn việc hay hết việc" (`io.busy`). Đây là quyết định thiết kế giảm độ phức tạp giao tiếp (Manager không cần theo dõi từng lệnh), đổi lại Manager/Client mất khả năng biết chi tiết tiến độ — đây chính là lý do bài toán "biết khi nào an toàn đọc kết quả" ở §5.1.7 phải giải bằng `fence` (chờ TOÀN BỘ xong) chứ không thể "chờ đúng lệnh mvout đó xong" (không có thông tin để làm việc này).

#### 5.2.1 — Manager forward `RoCCCommand` vào Gemmini

**Mục đích thiết kế**: đây là điểm nối type-boundary — bên trái ranh giới này (Manager) nói "ngôn ngữ AuRORA" (message NoC, credit, cfg_id), bên phải (Gemmini) nói "ngôn ngữ RoCC chuẩn" (`RoCCCommand` với `opcode/funct/rs1/rs2/rd`) — hoàn toàn không biết gì về khái niệm "AuRORA" hay "multi-tenant". Đây chính là điểm mạnh của kiến trúc: Gemmini được viết ra **độc lập hoàn toàn** với AuRORA (nó là 1 accelerator RoCC bình thường), AuRORA "mượn" nó mà không cần sửa 1 dòng code nào bên trong Gemmini — toàn bộ độ phức tạp multi-tenant nằm gọn ở Manager.

**Logic hoạt động**: hàm ở bước này chỉ đơn thuần chuyển tiếp — nhưng ẩn sau sự đơn giản đó là 1 chi tiết quan trọng: trường `opcode` trong lệnh, tại thời điểm rời khỏi Manager, **không còn là opcode gốc mà Client dùng nữa** — nó bị ghi đè thành 1 giá trị cố định mà Gemmini đã đăng ký sẵn lúc khởi tạo hệ thống. Đây là hệ quả trực tiếp của bài toán "gián tiếp mã lệnh" đã giải ở §5.1.3: Client dùng `RROPCn` để chọn accelerator theo cfg_id của RIÊNG NÓ, nhưng Gemmini (dùng chung cho nhiều Client khác nhau theo thời gian) chỉ biết đúng 1 opcode cố định của chính nó — Manager là nơi "phiên dịch" giữa 2 không gian tên khác nhau này.

```cpp
gemmini->pushCmd(cmd);   // cmd.opcode đã được Manager ghi đè thành opcode cố định của Gemmini
```

🎯 **Điểm mấu chốt**: nếu debug thấy opcode "khác" giữa lúc Client gửi và lúc Gemmini nhận, đây là hành vi ĐÚNG theo thiết kế, không phải bug.

#### 5.2.2 — Reservation Station: `alloc` + tính dependency

**Mục đích thiết kế**: đây là bước hiện thực hoá trực tiếp nguyên lý số 2 ở §5.2.0. Vấn đề cụ thể cần giải: Gemmini có bộ nhớ on-chip riêng (Scratchpad, Accumulator) mà nhiều lệnh liên tiếp có thể cùng đọc/ghi — nếu cho phép chúng thực thi hoàn toàn tự do (không kiểm tra gì), 1 lệnh `COMPUTE` đọc dữ liệu TRƯỚC KHI lệnh `LOAD` tương ứng kịp ghi xong sẽ cho ra kết quả sai (hazard kinh điển "đọc trước khi ghi" — RAW). Nhiệm vụ của bước này là **phát hiện trước** những cặp lệnh có nguy cơ này, ghi lại thành 1 "ràng buộc phải chờ", để bước issue (§5.2.3) biết lệnh nào chưa được phép chạy.

**Logic hoạt động chi tiết**: với MỖI lệnh mới muốn vào hệ thống, phải so sánh vùng địa chỉ on-chip nó sẽ đụng tới với vùng địa chỉ của MỌI lệnh khác đang còn "sống" (chưa hoàn tất) trong cả 3 hàng đợi — nếu có chồng lấn, lệnh mới phải "ghi nợ" (đánh dấu phụ thuộc) vào đúng lệnh cũ đó. Đáng chú ý: việc kiểm tra này xảy ra **1 lần duy nhất tại thời điểm lệnh vào hệ thống** (không phải kiểm tra lại liên tục) — vì tập hợp "lệnh đang sống" chỉ có thể tăng dần trong khoảng thời gian giữa lúc lệnh mới vào và lúc nó rời hàng đợi để bắt đầu kiểm tra, nên kiểm tra 1 lần tại đúng thời điểm vào là đủ.

```cpp
struct RSEntry {
    RoCCCmd cmd;
    std::bitset<48> depsLd, depsEx, depsSt;
    int issueId;
};

bool ReservationStation::alloc(RoCCCmd cmd) {
    if (ldQueue.full() || exQueue.full() || stQueue.full()) return false;
    RSEntry entry; entry.cmd = cmd; entry.issueId = nextIssueId++;
    AddrRange newRange = extractLocalAddrRange(cmd);
    for (auto &existing : allEntries()) {
        if (overlaps(newRange, existing.range)) markDependency(entry, existing);
    }
    pushToCorrectQueue(entry);
    return true;
}
```

🎯 **Điểm mấu chốt**: độ sâu 3 hàng đợi KHÔNG bằng nhau (`ld=8, ex=16, st=4`) — đây không phải con số tuỳ tiện mà phản ánh đúng nguyên lý 1 (access/execute decoupling): hàng đợi tính toán (`ex`) sâu nhất vì đó là tài nguyên "đắt" nhất (systolic array), cần được giữ luôn bận bằng cách cho phép nhiều lệnh tính toán xếp hàng sẵn; hàng đợi ghi ra (`st`) nông nhất vì kết quả ra thường ít lệnh hơn số lệnh đọc vào/tính toán trong 1 workload ma trận điển hình.

#### 5.2.3 — Issue từ Reservation Station tới 3 Controller

**Mục đích thiết kế**: đây là nơi "lý thuyết phụ thuộc" ở bước trước chuyển thành "hành động thật" — mỗi chu kỳ, hệ thống phải tự hỏi lại: trong số các lệnh đang chờ, lệnh nào GIỜ ĐÃ đủ điều kiện chạy (mọi lệnh nó phụ thuộc đã hoàn tất)? Đây chính là cơ chế "tái đánh giá liên tục" điển hình của mọi bộ điều phối out-of-order.

**Logic hoạt động**: 3 quyết định "có issue hay không" cho 3 loại lệnh (`ld/ex/st`) là **hoàn toàn độc lập nhau về mặt logic** — 1 lệnh Load có thể được issue trong khi 1 lệnh Compute khác đang chờ, và ngược lại, miễn mỗi lệnh riêng lẻ đã hết ràng buộc của chính nó. Đây chính là điểm hiện thực hoá "song song hoá" thật sự trong hệ thống — nếu bạn code 3 quyết định này tuần tự trong CÙNG 1 hàm/1 event (thay vì 3 luồng độc lập thật sự có thể chạy cùng lúc), về mặt giá trị tính toán vẫn đúng, nhưng về mặt ĐO HIỆU NĂNG sẽ sai — vì bạn đã vô tình serialize hoá thứ đáng lẽ phải song song.

```cpp
void ReservationStation::tryIssueReady() {
    if (!ldQueue.empty() && ldQueue.front().depsLd.none())
        loadController->issue(ldQueue.pop());
    if (!exQueue.empty() && exQueue.front().depsEx.none())
        exController->issue(exQueue.pop());
    if (!stQueue.empty() && stQueue.front().depsSt.none())
        storeController->issue(stQueue.pop());
}
```

🎯 **Điểm mấu chốt**: dựng 3 `EventFunctionWrapper` độc lập cho 3 Controller (không dùng chung 1 hàm `tick()` gộp) để giữ đúng tính song song này trong mô phỏng.

#### 5.2.4 — Load path: DMA đọc từ hệ thống bộ nhớ

**Mục đích thiết kế**: đây là "cửa ngõ nạp nguyên liệu" — dữ liệu ma trận nằm ở DRAM/L2 (xa, chậm) phải được kéo vào Scratchpad on-chip (gần, nhanh) trước khi tính toán có thể bắt đầu. Vì đây là con đường có độ trễ CAO NHẤT trong toàn bộ pipeline Gemmini (DRAM access latency luôn lớn hơn nhiều so với SRAM access hay tính toán nội bộ), bước này được thiết kế để **không bao giờ chặn các luồng khác** trong khi chờ — đây chính là lý do tồn tại `xactTracker` (cho phép nhiều yêu cầu đọc cùng bay trên đường truyền, thay vì đọc xong 1 cái mới được phát cái tiếp theo).

**Logic hoạt động chi tiết**: trước khi đi ra bộ nhớ hệ thống (chậm), luôn thử tra cứu bộ nhớ đệm dịch địa chỉ tầng 1 trước (nhanh, cục bộ) — chỉ khi tầng 1 không có mới phải "hỏi lên" tầng 2 (chậm hơn, phải qua Manager). Đây là nguyên lý phân tầng bộ nhớ đệm kinh điển: đặt cái rẻ/nhanh gần nơi dùng, cái đắt/chậm dùng chung cho nhiều nơi.

```cpp
void LoadController::execute(RSEntry entry) {
    Addr pa;
    if (frontendTLB.lookup(entry.cmd.va, pa)) { issueDma(pa, entry); return; }
    manager->mmuWalker->translate(entry.cmd.va, [this, entry](Addr pa, bool ok) {
        if (!ok) { raiseException(entry); return; }
        frontendTLB.insert(entry.cmd.va, pa);
        issueDma(pa, entry);
    });
}
void LoadController::issueDma(Addr pa, RSEntry entry) {
    int srcId = xactTracker.allocId();
    dma->sendRead(pa, entry.cmd.numBytes, srcId, [this, entry](Data d) {
        scratchpad.write(entry.cmd.localAddr, d, entry.cmd.accumulate);
        reservationStation->markCompleted(entry.issueId, QueueType::LD);
    });
}
```

🎯 **Điểm mấu chốt**: dùng callback bất đồng bộ (như mẫu trên), TUYỆT ĐỐI không code kiểu chờ đồng bộ (blocking) — nếu không, lợi ích của việc cho phép nhiều request đọc cùng lúc sẽ mất trắng, độ trễ đo được sẽ cao hơn thực tế rất nhiều.

#### 5.2.5 — Execute path: Mesh systolic array

**Mục đích thiết kế**: đây là nơi giá trị thật sự được tạo ra — phép nhân ma trận qua 1 mảng phần tử xử lý (PE) xếp lưới, mỗi PE làm 1 phép nhân-cộng nhỏ, dữ liệu "chảy" qua mảng theo nhịp đồng hồ. Thiết kế "weight-stationary" (trọng số đứng yên trong PE, dữ liệu chảy qua) là lựa chọn tối ưu cho việc TÁI SỬ DỤNG trọng số nhiều lần mà không phải nạp lại — đây là lý do `PRELOAD` (nạp trọng số) và `COMPUTE` (chảy dữ liệu qua) là 2 lệnh TÁCH RỜI: nếu cùng 1 bộ trọng số dùng cho nhiều phép tính liên tiếp, chỉ cần `PRELOAD` 1 lần rồi `COMPUTE` nhiều lần, tiết kiệm rất nhiều thời gian nạp lại.

**Logic hoạt động**: độ trễ của 1 lệnh `COMPUTE` không cố định — nó phụ thuộc kích thước mảng PE (`meshRows`/`meshCols`) VÀ kích thước "tile" cấu hình được — vì dữ liệu phải "chảy" qua toàn bộ chiều dài mảng trước khi kết quả cuối cùng ra khỏi đầu kia.

```cpp
void ExController::execute(RSEntry entry) {
    Data operands = scratchpad.read(entry.cmd.localAddr);
    int latencyCycles = estimateMeshLatency(entry.cmd);  // ~O(meshRows+meshCols+tileRows*tileCols)
    schedule(completeEvent, curTick() + cyclesToTicks(latencyCycles));
    pendingEntry = entry; pendingOperands = operands;
}
void ExController::onMeshComplete() {
    Data result = computeMeshResult(pendingOperands);
    scratchpad.writeAccumulator(pendingEntry.cmd.localAddr, result, pendingEntry.cmd.accumulate);
    reservationStation->markCompleted(pendingEntry.issueId, QueueType::EX);
}
```

🎯 **Điểm mấu chốt**: nếu model của bạn coi mỗi lệnh Ex độc lập hoàn toàn (tính riêng độ trễ `PRELOAD` + độ trễ `COMPUTE` cộng dồn cho mỗi cặp), bạn sẽ đánh giá SAI hiệu năng của pattern tái sử dụng trọng số — thực tế `PRELOAD` gần như miễn phí về thời gian khi đang ở chế độ weight-stationary tái sử dụng. Đây cũng là gốc rễ của ngoại lệ `solitaryPreload` sẽ gặp lại ở §5.2.9.

#### 5.2.6 — Store path: DMA ghi ra hệ thống bộ nhớ

**Mục đích thiết kế**: đây là "cửa ngõ duy nhất" kết quả tính toán rời khỏi phạm vi on-chip để CPU (hoặc bất kỳ ai khác trong hệ thống) có thể nhìn thấy — hệ quả trực tiếp của nguyên lý 3 (§5.2.0): vì Gemmini không "nói" chi tiết tiến độ ra ngoài, con đường DUY NHẤT để 1 kết quả "tồn tại theo nghĩa CPU nhìn thấy được" là phải thật sự đi qua bước ghi bộ nhớ này — không có "đường tắt" nào khác (không qua `rd`, không qua bất kỳ kênh nào khác).

**Logic hoạt động**: nếu dữ liệu lấy từ Accumulator (khác Scratchpad thô), có thể cần áp thêm 1 bước xử lý hậu kỳ nhẹ (activation/scale — ví dụ ReLU) TRƯỚC khi ghi ra — đây là tính năng tiện ích giúp workload mạng nơ-ron không cần 1 lệnh riêng chỉ để áp activation.

```cpp
void StoreController::execute(RSEntry entry) {
    Data d = scratchpad.readAccumulator(entry.cmd.localAddr);
    if (entry.cmd.isMvoutFromAcc) d = applyActivationAndScale(d, entry.cmd.actConfig);
    Addr pa;
    if (!frontendTLB.lookup(entry.cmd.va, pa)) {
        manager->mmuWalker->translate(entry.cmd.va, [this, entry, d](Addr pa, bool ok) {
            if (ok) { frontendTLB.insert(entry.cmd.va, pa); issueDmaWrite(pa, d, entry); }
        });
        return;
    }
    issueDmaWrite(pa, d, entry);
}
void StoreController::issueDmaWrite(Addr pa, Data d, RSEntry entry) {
    int srcId = xactTracker.allocId();
    dma->sendWrite(pa, d, srcId, [this, entry]() {
        reservationStation->markCompleted(entry.issueId, QueueType::ST);
    });
}
```

⚠️ **Đây là ranh giới trực tiếp nối với §5.1.7**: hoàn tất `dma->sendWrite()` ở bước này **không đồng nghĩa** phía CPU đã an toàn đọc lại — đó là lý do toàn bộ cơ chế `fence` ở §5.1.7 tồn tại. Khi viết testcase kiểm tra riêng cho race-condition này, cố tình đặt độ trễ DMA đủ lớn để buộc lộ ra bug nếu có (mô phỏng "quá nhanh" một cách tình cờ có thể che giấu race-condition thật).

#### 5.2.7 — Xoá dependency, mở khoá lệnh đang chờ

**Mục đích thiết kế**: đây là "mặt kia" của bước 5.2.2 — nếu 5.2.2 là nơi TẠO ràng buộc, đây là nơi GỠ ràng buộc khi điều kiện đã thoả. Không có bước này, mọi ràng buộc tạo ra ở 5.2.2 sẽ tồn tại vĩnh viễn, hệ thống deadlock ngay từ lệnh phụ thuộc đầu tiên.

**Logic hoạt động**: khi 1 lệnh bất kỳ (thuộc BẤT KỲ loại nào trong 3 loại) báo hoàn tất, phải quét TOÀN BỘ các lệnh còn đang chờ ở CẢ 3 hàng đợi (không chỉ hàng đợi cùng loại) để gỡ đúng bit tương ứng — vì phụ thuộc có thể xảy ra CHÉO loại (1 lệnh `STORE` hoàn toàn có thể phụ thuộc 1 lệnh `COMPUTE`, không chỉ phụ thuộc lệnh `LOAD` cùng loại với chính "nhóm" xử lý dữ liệu).

```cpp
void ReservationStation::markCompleted(int issueId, QueueType type) {
    for (auto &entry : allEntries()) {
        entry.depsLd.reset(issueId); entry.depsEx.reset(issueId); entry.depsSt.reset(issueId);
    }
    removeFromQueue(type, issueId);
}
```

🎯 **Điểm mấu chốt**: quét cả 3 loại bit bất kể lệnh vừa xong thuộc loại nào — bỏ sót quét chéo là lỗi phổ biến nhất khi mới implement dependency tracking, và hậu quả (deadlock giả ở 1 số lệnh nhất định) rất khó phát hiện qua test thông thường vì phần lớn workload không rơi đúng vào pattern phụ thuộc chéo loại.

#### 5.2.8 — `FLUSH_CMD`

**Mục đích thiết kế**: đây là công cụ "tự dọn dẹp" mà Gemmini cung cấp cho phần mềm điều khiển nó — nhưng CHỦ ĐÍCH thiết kế quan trọng cần hiểu: Gemmini (được viết độc lập, không biết gì về khái niệm multi-tenant của AuRORA — như đã nói ở §5.2.1) không thể tự động biết "khi nào 1 tenant kết thúc" để tự dọn — nó chỉ cung cấp CÔNG CỤ, còn AI gọi công cụ đó và GỌI KHI NÀO là trách nhiệm của tầng phía trên (ở đây là chuỗi rerocc_release, §5.1.8).

**Logic hoạt động**: đây là 1 lệnh đặc biệt, xử lý NGAY khi vào hệ thống, không qua toàn bộ cơ chế dependency-tracking phức tạp (§5.2.2-5.2.3) — hợp lý vì bản chất của nó là "xoá sạch", không có khái niệm phụ thuộc dữ liệu nào áp dụng được cho 1 hành động xoá toàn bộ.

```cpp
void GemminiAccel::pushCmd(RoCCCmd cmd) {
    if (cmd.funct == FLUSH_CMD) { frontendTLB.flushAll(); return; }
    rawCmdQueue.push(cmd);
}
```

⚠️ **Đây là điểm nối với lỗ hổng bảo mật đã cảnh báo ở §5.1.8**: chính vì Gemmini KHÔNG tự biết khi nào cần tự gọi `FLUSH_CMD`, và Manager (ở §5.1.8) cũng KHÔNG tự động chèn lệnh này khi release (đã xác nhận grep RTL — không có đường code nào làm việc đó), nên trách nhiệm gọi đúng lúc **hoàn toàn thuộc về tầng runtime/OS bạn tự viết**. Đây không phải sơ suất của thiết kế gốc — là ranh giới trách nhiệm rõ ràng có chủ đích (Gemmini là accelerator "thuần", AuRORA là lớp multi-tenant "mượn" nó) — nhưng bạn phải tự implement đúng phần trách nhiệm của lớp trên.

#### 5.2.9 — Tổng hợp `io.busy`

**Mục đích thiết kế**: đây là hiện thực hoá trực tiếp nguyên lý 3 (§5.2.0) — 1 tín hiệu boolean DUY NHẤT phải "gánh" trách nhiệm phản ánh trung thực trạng thái của TOÀN BỘ hệ thống con bên trong Gemmini (hàng đợi lệnh thô, bộ giải nén vòng lặp, scoreboard, bộ nhớ on-chip) — chỉ cần 1 trong các thành phần này còn việc dở dang mà tín hiệu lại báo "rảnh", toàn bộ chuỗi đồng bộ hoá ở §5.1.7-5.1.8 (vốn hoàn toàn tin tưởng tín hiệu này) sẽ đưa ra quyết định sai — cho phép CPU đọc dữ liệu chưa sẵn sàng, hoặc cho phép tenant mới acquire trong khi tenant cũ chưa thật sự dừng.

**Logic hoạt động chi tiết — vì sao có ngoại lệ `solitaryPreload`**: đây là 1 quyết định thiết kế tinh tế, đáng dành riêng 1 đoạn để hiểu đúng bản chất thay vì chỉ nhớ máy móc. Nhắc lại từ §5.2.5: `PRELOAD` và `COMPUTE` là cặp lệnh phối hợp cho pattern tái sử dụng trọng số — phần mềm hoàn toàn có thể chủ đích `PRELOAD` 1 lần rồi ĐỢI (chưa vội gọi `COMPUTE` ngay) để dành chỗ cho phép tái sử dụng ở NHIỀU thời điểm khác nhau trong chương trình. Nếu hệ thống coi trạng thái "có đúng 1 `PRELOAD` đang chờ, chưa có `COMPUTE` đi kèm" là "bận", thì bất kỳ ai gọi `fence` trong khoảng thời gian phần mềm ĐANG CHỦ ĐÍCH giữ trạng thái này sẽ bị treo vô lý (dù không có gì thật sự "đang xử lý", chỉ là 1 trạng thái chờ có chủ đích) — đây chính là lý do thiết kế phải "nhìn xuyên" qua trường hợp đặc biệt này và báo "rảnh" dù về mặt số lượng entry KHÔNG bằng 0.

```cpp
bool GemminiAccel::isBusy() const {
    return !rawCmdQueue.empty() || loopUnroller.isBusy()
        || reservationStation.isBusy() || scratchpad.isBusy();
}
bool ReservationStation::isBusy() const {
    if (allEntries().size() == 1 && allEntries()[0].isSolitaryPreload()) return false;
    return !allEntries().empty();
}
bool Scratchpad::isBusy() const {
    return writer.busy || reader.busy
        || !writeIssueQueue.empty() || !writeNormQueue.empty() || !writeScaleQueue.empty();
}
```

🎯 **Điểm mấu chốt cuối cùng, quan trọng nhất toàn bộ §5.2**: nếu code sai ngoại lệ này (coi solitary-preload là "busy" thay vì "rảnh"), hệ quả là `fence`/`release` **treo vĩnh viễn** trong đúng 1 tình huống hiếm nhưng có thật (weight-stationary reuse) — loại bug này gần như KHÔNG BAO GIỜ lộ ra qua testcase ngẫu nhiên thông thường (vì đa số workload không cố tình giữ `PRELOAD` chờ lâu), nhưng sẽ gây deadlock thật trong 1 workload production cụ thể dùng đúng pattern tái sử dụng trọng số. Bắt buộc viết 1 unit test riêng dựng đúng tình huống này (`PRELOAD` rồi gọi `fence` ngay, KHÔNG có `COMPUTE` theo sau) trước khi coi §5.2 hoàn thành.

### 6.1 Danh sách quyết định thiết kế còn mở (❓), cần chốt trước khi code

| # | Quyết định | Vị trí | Khuyến nghị |
|---|---|---|---|
| 1 | CSR thật vs PIO/MMIO | §3.2 | CSR thật, nếu cần validate chéo RTL |
| 2 | Adapter `ThreadContext` cho `Walker` | §3.10 | Viết `ShadowThreadContext` tối giản |
| 3 | Manager↔Gemmini dùng Port hay con trỏ trực tiếp | §3.9 | Con trỏ trực tiếp |
| 4 | Port `Manager` cho FLUSH_CMD tầng 1 có cần forward tự động khi release không (hiện KHÔNG, đúng RTL) | §5.1 bước 8, §5.2 bước 7 | Giữ nguyên tách biệt, KHÔNG tự động — đúng RTL, dù có rủi ro bảo mật nếu firmware quên gọi |

### 6.2 Danh sách rủi ro/case chưa kiểm chứng đầy đủ, cần bạn tự test bằng RTL simulation

1. **`xs1=0, xs2=1`** — nghi vấn bug tại `Client.scala:50-52`, không có testcase chuẩn nào phủ tới.
2. **Race "release rồi acquire lại ngay khi cả pool đang bận"** — `test.c` chuẩn tự exercise case này nhưng không có logic retry toàn cục nếu thất bại.
3. **MEM Req Throttler / `rerocc_memrate`** — paper MICRO'23 mô tả nhưng **không tồn tại** trong `ucb-bar/rerocc` public. Nếu cần, phải tự thiết kế, không có RTL tham chiếu.

### 6.3 Glossary

| Thuật ngữ | Giải thích |
|---|---|
| `cfg` / `cfgId` | 1 trong 16 "ngăn" ở Client, đại diện 1 accelerator đã/đang acquire |
| Tracker | Tên gọi khối quản lý 16 `cfg` này (theo Figure 2 paper) |
| Shadow Registers | Bản sao `status`(mstatus)/`ptbr` của CPU, giữ ở Manager để accelerator dịch địa chỉ đúng |
| ROB (docs) / Reservation Station (code) | Cùng 1 khối — bộ scoreboard 3 hàng đợi của Gemmini |
| `io.busy` | Tín hiệu tổng hợp duy nhất Manager dùng để biết accelerator "xong việc" |

### 6.4 File đính kèm liên quan

- `aurora_sq.puml` — sequence diagram Client↔Manager (8 bước, đã pass 17/17 case firmware)
- `gemmini_sq.puml` — sequence diagram Manager↔Gemmini (9 bước)
- `RoccCPU_sa.puml`, `client_sa.puml`, `manager_sa.puml`, `gemmini_sa.puml` — structure diagram 4 khối, port đã chuẩn hoá theo gem5 API
- `rerocc_source_review.md` — review kỹ thuật gốc, trích dẫn RTL đầy đủ file:line cho mọi khẳng định trong tài liệu này
