# Tài Liệu Thiết Kế Kỹ Thuật Hệ Thống: AuRORA Architecture (Version 1.0 - Outsource Specification)

---

## 1. Tổng Quan Kiến Trúc Hệ Thống (System Architecture)

Sơ đồ kiến trúc tổng thể mô tả mối quan hệ trực giao và cấu trúc phân rã module của hệ thống **gem5 System-on-Chip (AuRORA Architecture)**:

![System Architecture Diagram](placeholder_system_architecture.png)

### 1.1. Chi Tiết Các Khối Chức Năng Cốt Lõi
* **Host CPU Subsystem**:
  * **RISC-V CPU Core**: Đóng vai trò là master điều phối trung tâm, thực thi các lệnh chương trình chính, thao tác ghi/đọc thanh ghi CSR tùy chỉnh qua các lệnh system / custom swap và phát lệnh RoCC.
  * **AuRORA Client Tracker**: Quản lý tập hợp 16 thanh ghi cấu hình (`cfg[acq|mgr_id]`), 4 thanh ghi mã lệnh `RROPC`, 16 pool tín dụng (`credit pools`) độc lập và một FSM điều khiển chung (`cfg_acq_state`).
  * **Instruction Sender**: Bộ tuần tự hóa phần cứng (Serializer 1-3 beats) chịu trách nhiệm đóng gói các lệnh tùy chỉnh và thông điệp điều khiển thành các gói tin NoC (`mAcquire`, `mInst`, `mUStatus`, `mUPtbr`, `mUnbusy`, `mRelease`).
* **Memory Subsystem**:
  * **Main Memory DRAM & DRAM Controller**: Cung cấp không gian bộ nhớ chính phục vụ các yêu cầu đọc/ghi dữ liệu lớn từ Accelerator thông qua giao thức AXI/TileLink.
* **System Interconnect (Garnet NoC)**:
  * Đóng vai trò là hạ tầng mạng truyền thông trên chip (Network-on-Chip), định tuyến các gói tin yêu cầu và phản hồi giữa **Host CPU Subsystem** và **Accelerator Subsystem Tenant** bằng cơ chế Virtual Channels.
* **Accelerator Subsystem Tenant**:
  * **AuRORA Manager**: Quản lý trạng thái thực thi của Accelerator qua các trạng thái FSM (`s_idle`, `s_active`, `s_rel_wait`, `s_sfence`, `s_unbusy`), duy trì Shadow Registers (trạng thái hệ thống và con trỏ bảng trang được đồng bộ) và quản lý hàng đợi lệnh `inst_q` (độ sâu `depth = 4`).
  * **Hardware MMU Subsystem**: Tích hợp module PTW (Page Table Walker) chuẩn Rocket-Chip, hỗ trợ cơ chế dịch địa chỉ ảo sang địa chỉ vật lý (`VA -> PA`) trực tiếp dựa trên Shadow PTBR.
  * **Global Buffer & Gemmini Accelerator**: Khối đệm dữ liệu toàn cục kết hợp với engine tính toán ma trận chuyên dụng (`Gemini Accel`) để xử lý các thuật toán tăng tốc AI/HPC.

---

## 2. Đặc Tả Giao Thức Thông Điệp & Bản Tin NoC (Bus Protocol & Opcodes)

Hệ thống giao tiếp thông qua các mã định danh bản tin (Opcode) chuẩn hóa trên giao diện bus:

* **Request Opcodes (`m*` - Từ Host gửi tới Accelerator)**:
  * `mAcquire` (`0` - 1 beat): Yêu cầu chiếm quyền điều khiển (acquire) một Manager cụ thể.
  * `mInst` (`1` - 1 đến 3 beats): Truyền gói lệnh tùy chỉnh RoCC kèm theo các giá trị thanh ghi toán hạng `rs1`, `rs2`.
  * `mUStatus` (`2` - 2 beats): Đồng bộ thanh ghi trạng thái người dùng (`mstatus[63:0]` và `mstatus[127:64]`).
  * `mUPtbr` (`3` - 1 beat): Đồng bộ thanh ghi con trỏ bảng trang `ptbr`.
  * `mRelease` (`4` - 1 beat): Thông điệp yêu cầu giải phóng quyền điều khiển Manager.
  * `mUnbusy` (`5` - 1 beat): Thông điệp rào chắn (Fence / Drain synchronization) đảm bảo toàn bộ lệnh cũ đã hoàn tất.

* **Response Opcodes (`s*` - Từ Accelerator phản hồi về Host)**:
  * `sAcqResp` (`0`): Phản hồi yêu cầu Acquire (`data(0) = 1` nếu thành công, `0` nếu thất bại do tranh chấp).
  * `sInstAck` (`1`): Xác nhận lệnh đã được nạp thành công vào hàng đợi lệnh (`inst_q`).
  * `sWrite` (`2` - 2 beats): Gửi kết quả ghi ngược về CPU (`data` ở beat 0, thanh ghi đích `rd` ở beat 1).
  * `sRelResp` (`3`): Xác nhận quá trình giải phóng tài nguyên và drain bộ đệm đã hoàn tất.
  * `sUnbusyAck` (`4`): Xác nhận lệnh rào chắn (Fence) đã được xử lý xong.

---

## 3. Luồng Sequence Diagram Tổng Thể

Sơ đồ tuần tự thể hiện toàn bộ các bước tương tác chuẩn từ lúc cấu hình, cấp phát, truyền lệnh, thực thi tính toán, đồng bộ đến khi giải phóng tài nguyên:

![Sequence Diagram](placeholder_sequence_diagram.png)

---

## 4. Giải Thích Chi Tiết Từng Luồng Thực Thi (Implementation Guide for Engineers)

Phần này phân tích chi tiết từng bước thực thi trong sơ đồ tuần tự để kỹ sư thiết kế phần cứng hoặc lập trình viên mô hình hóa có thể hiện thực hóa chính xác từng module.

---

### Giai Đoạn 1: `rerocc_acquire` (Cấp Phát Manager cho Client)
* **Mục đích**: Cho phép CPU yêu cầu chiếm quyền độc quyền một `Manager` thông qua việc ghi thanh ghi CSR cấu hình `RRCFGn`.
* **Quy trình thực thi chi tiết**:
  1. CPU thực hiện lệnh `swap_csr(RRCFGn, ACQ=1 | mgr_id)` thông qua cổng `Client_CpuPort`.
  2. **Kiểm tra tại Tracker**: 
     * Kiểm tra trạng thái FSM chung `cfg_acq_state == s_idle` (FSM này dùng chung cho tất cả 16 cấu hình) và kiểm tra xem `mgr_id` có tồn tại hợp lệ trong danh sách cấu hình của hệ thống (`edge.mParams.managers`) hay không.
   * **Nhánh Thất Bại (`cfg_acq_state != s_idle` hoặc `mgr_id` không hợp lệ)**:
     * Tracker **không** gửi bất kỳ gói tin nào ra NoC (tránh lãng phí chu kỳ round-trip qua mạng).
     * Trả về kết quả cho CPU ngay lập tức qua `Client_CpuPort`: thanh ghi CSR readback giữ nguyên bit `acq = 0`.
   * **Nhánh Thành Công (FSM rảnh, ID hợp lệ)**:
     * Tracker cập nhật trạng thái: `cfg_acq_state := s_acq`, ghi nhận ID cấu hình đang xử lý và `mgr_id` tương ứng.
     * Kích hoạt `req_arb.in(0)` đẩy gói tin `mAcquire(client_id=n, manager_id=mgr_id)` qua `Client_NocPort` -> `Garnet NoC` -> `Mgr_NocPort` -> `Mgr_Core`.
     * **Kiểm tra trạng thái tại Manager Core**:
       * Nếu `Manager.state != s_idle` (đang bị chiếm giữ bởi client khác): Manager trả về phản hồi `sAcqResp(data(0)=0)` (Denied). Phản hồi đi ngược lại qua NoC về Client, cập nhật `cfg_acq_state := s_idle` và trả về `acq=0` cho CPU (FAIL). *Lưu ý phần mềm: Cần cơ chế vòng lặp thử các Accelerator ID khác trong pool.*
       * Nếu `Manager.state == s_idle`: Manager chuyển trạng thái sang `s_active`, gắn kết định danh `client := n`, và trả về `sAcqResp(data(0)=1)` (Granted). Client nhận phản hồi thành công, cập nhật `cfg_acq_state := s_idle`, set bít `acq := 1`, và bật cờ kích hoạt đồng bộ `cfg_updatestatus(n) := true` và `cfg_updateptbr(n) := true`.

---

### Giai Đoạn 2: Tự Động Đồng Bộ Shadow Registers (mUStatus & mUPtbr)
* **Mục đích**: Ngay sau khi acquire thành công, hệ thống tự động đồng bộ ngữ cảnh hệ thống (`mstatus`) và con trỏ bảng trang (`ptbr`) của CPU sang Shadow Registers phía Manager.
* **Quy trình thực thi chi tiết**:
  1. Hai giao dịch này được kích hoạt tuần tự và tự động nhờ các cờ `cfg_updatestatus` và `cfg_updateptbr` đã được bật ở Giai đoạn 1 (chạy qua FSM chung `cfg_acq_state`: `s_status0 -> s_status1 -> s_ptbr -> s_idle`).
  2. **Truyền `mUStatus` (2 beats)**:
     * Beat 0: Gửi `mstatus[63:0]` qua NoC tới Manager.
     * Beat 1: Gửi `mstatus[127:64]` qua NoC tới Manager.
     * Tại Manager: Giá trị `status` được cập nhật thành `Cat(beat1, beat0)` *(Chỉ áp dụng cập nhật khi `!inst_q.deq.valid && !io.busy` để đảm bảo không ghi đè giữa chừng lệnh đang chạy)*.
  3. **Truyền `mUPtbr` (1 beat)**:
     * Gửi giá trị con trỏ bảng trang `ptbr` qua NoC.
     * Tại Manager: Cập nhật giá trị `ptbr := beat0` *(Áp dụng điều kiện tương tự: chỉ khi không có lệnh active)*.
  4. *Lưu ý kỹ thuật*: Manager cũng tự động re-sync lại `status`/`ptbr` mỗi khi CPU thay đổi ngữ cảnh (ví dụ context switch), áp dụng cho mọi cấu hình đang acquire chứ không riêng lúc khởi tạo ban đầu (`Client.scala:281-282`).

---

### Giai Đoạn 3: `rerocc_assign` (Ánh Xạ Opcode Tùy Chỉnh)
* **Mục đích**: Cấu hình ánh xạ mã lệnh RoCC tùy chỉnh thông qua thanh ghi CSR `RROPC`.
* **Quy trình thực thi chi tiết**:
  1. CPU thực hiện ghi giá trị `write_rr_csr(RROPC_k, cfg_id=n)` (với `k = 0..3`, tương ứng với 4 khe giải mã decode slot).
  2. `Client_CpuPort` chuyển lệnh tới Tracker: cập nhật trực tiếp `csr_opc(k) := n` mà không cần đi qua FSM chính.
  3. Trả về tín hiệu `done` ngay lập tức cho CPU (không cần ACK từ Manager vì đây là thao tác cấu hình local thuần túy).
  4. Kể từ thời điểm này, mọi lệnh RoCC tùy chỉnh sử dụng trường opcode `bit[6:5] = k` phát ra từ CPU sẽ được định tuyến tự động tới `cfg n` và Manager tương ứng.

---

### Giai Đoạn 4: Phát Lệnh RoCC Custom Instruction (Data-Plane)
* **Mục đích**: Thực thi gửi các lệnh tính toán tùy chỉnh từ CPU xuống Accelerator.
* **Quy trình thực thi chi tiết**:
  1. CPU phát lệnh `ROCC_INSTRUCTION(opcode=k, rs1?, rs2?, rd?)`.
  2. **Kiểm tra điều kiện tại Tracker**:
     * Lấy `cfg_id = csr_opc(k)`.
     * Kiểm tra các điều kiện: `credit_available (cfg_credits(cfg_id) != 0 && cfg.acq)`, `status_ready`, `ptbr_ready`, và `cfg_acq_state == s_idle`.
   * **Trường hợp không thỏa điều kiện (Hết credit / đang bận)**:
     * Trả về `io.cmd.ready = false`, CPU bị stall (backpressure, RoCC busy). *Cảnh báo Head-of-line blocking*: `cfg_acq_state != s_idle` có thể chặn lệnh của các accelerator khác dùng chung Client.
   * **Trường hợp thỏa điều kiện (OK)**:
     * Giảm số lượng credit: `cfg_credits(cfg_id)--`.
     * Trả về tín hiệu ACK không chặn (`TRUE`) cho CPU.
     * Kích hoạt `InstructionSender` (trạng thái `s_inst`):
       * Gửi beat 0 chứa mã lệnh `inst` qua NoC.
       * Nếu `inst.xs1 == true`: Gửi tiếp beat 1 chứa toán hạng `rs1` qua NoC.
       * Nếu `inst.xs2 == true`: Gửi beat tiếp theo chứa toán hạng `rs2` qua NoC *(Lưu ý kiểm tra kỹ logic phần cứng gốc đối với trường hợp `xs1=0` và `xs2=1` để tránh bug serializer)*.
     * Tại Manager (`Mgr_NocPort` -> `Mgr_Core`):
       * Kiểm tra khẳng định `state == s_active`, đẩy lệnh `RoCCCommand` vào hàng đợi `inst_q` (độ sâu `depth = 4`).
       * Phát tín hiệu xác nhận `sInstAck` gửi ngược về Client (Xác nhận lệnh đã được nạp vào queue, **không** phải là đã thực thi xong).
       * Tại Client: Nhận `sInstAck` và hoàn trả credit ngay lập tức: `cfg_credits(cfg_id)++`.

---

### Giai Đoạn 5: Thực Thi Tính Toán & Dịch Địa Chỉ (Execution & Address Translation)
* **Mục đích**: Xử lý tính toán trên Accelerator kết hợp cơ chế dịch địa chỉ ảo sang địa chỉ vật lý.
* **Quy trình thực thi chi tiết**:
  1. `Mgr_Core` lấy lệnh (`Pop`) tuần tự theo thứ tự FIFO từ `inst_q`.
  2. Kích hoạt phần cứng tính toán: `Gemini Accel` bắt đầu chạy dựa trên mã `inst.opcode` cố định của accelerator.
  3. **Nhóm Dịch Địa Chỉ (Address Translation via MMU)**:
     * `Gemini` gửi yêu cầu dịch địa chỉ `VA -> PA` tới `Mgr_MMU` thông qua `PTW.io.requestor` (sử dụng Shadow PTBR đã đồng bộ).
     * Nếu cấu hình `l2TLBEntries = 0` / `nPTECacheEntries = 0` (mặc định): Hệ thống thực hiện walk page table trực tiếp từ bộ nhớ chính mà không qua cache trung gian.
     * `Mgr_MMU` trả về địa chỉ vật lý `PA` cho Gemini.
  4. **Truy xuất bộ nhớ qua Memory Sender**:
     * `Gemini` gửi yêu cầu truy xuất bộ nhớ (`PA`) qua `MiniDCache` / `HellaMMIO` / `Mgr_MemSender`.
     * *Lưu ý thiết kế gem5*: Trong thiết kế gốc của `ucb-bar/rerocc`, request đi thẳng qua crossbar TileLink (`tlXbar`) mà không có bộ điều tiết băng thông (throttler/shaper) xen giữa. Nếu cần kiểm soát throttling trong mô hình gem5, đội ngũ thiết kế cần tự bổ sung module này.
  5. Sau khi hoàn tất tính toán, `Gemini` trả kết quả về cho `Mgr_Core`.

---

### Giai Đoạn 6: Ghi Kết Quả Trả Về CPU (Writeback)
* **Mục đích**: Gửi kết quả tính toán (`sWrite`) từ Accelerator về lại cho thanh ghi đích (`rd`) trên CPU.
* **Quy trình thực thi chi tiết**:
  1. `Mgr_Core` đẩy dữ liệu phản hồi qua `Mgr_NocPort` (`resp_arb.in(2)`) gồm 2 beats:
     * **Beat 0**: Truyền dữ liệu `data`. Tại Client (`Client_NocPort` -> `Tracker`), tạm lưu vào biến đệm `resp_data := data` (`resp_first`).
     * **Beat 1**: Truyền định danh thanh ghi đích `rd`. Tại Client, kết hợp `io.resp.bits.rd := rd` và `io.resp.bits.data := resp_data` (`resp_last`).
  2. Tracker thực hiện writeback giá trị vào thanh ghi đích `rd` của CPU Core, hoàn tất một chu kỳ giao dịch lệnh.

---

### Giai Đoạn 7: `rerocc_fence` (Rào Chắn Đồng Bộ Hóa Tùy Chọn)
* **Mục đích**: Đảm bảo toàn bộ các lệnh đang tồn đọng trong hàng đợi hoặc accelerator đã được thực thi xong (drain) trước khi CPU tiếp tục tiến trình tiếp theo.
* **Quy trình thực thi chi tiết**:
  1. CPU thực hiện ghi CSR `write_rr_csr(RRBAR, cfg_id=n)` kết hợp lệnh Assembly `fence`.
  2. Tại Tracker: Cập nhật trạng thái `cfg_fence_state(n) := f_req` (chỉ gửi tín hiệu `mUnbusy` khi `!inst_sender.busy && !io.cmd.valid`).
  3. Gửi thông điệp `mUnbusy(client_id=n)` qua NoC tới Manager.
  4. Tại Manager: Chuyển trạng thái sang `s_unbusy`. Chờ đến khi điều kiện `!io.busy(accel) && inst_q.count == 0` được thỏa mãn hoàn toàn.
  5. Manager gửi phản hồi `sUnbusyAck` và chuyển lại trạng thái `s_active`.
  6. Phản hồi đi qua NoC về Client, cập nhật `cfg_fence_state(n) := f_idle`.
  7. CPU nhận tín hiệu `fence retire`, đảm bảo thứ tự nhất quán bộ nhớ theo chuẩn RISC-V.

---

### Giai Đoạn 8: `rerocc_release` (Giải Phóng Tài Nguyên Manager)
* **Mục đích**: Thu hồi quyền sử dụng Accelerator, dọn dẹp TLB và làm sạch bộ nhớ đệm trang để đảm bảo tính an toàn trong môi trường đa nhiệm (Multi-tenant isolation).
* **Quy trình thực thi chi tiết**:
  1. CPU ghi CSR `write_rr_csr(RRCFGn, 0)` để thực hiện giải phóng.
  2. Tại Client (`Client_CpuPort` -> `Tracker`): Cập nhật ngay lập tức `csr_cfg_next(n) := 0` (không cần chờ phản hồi `sRelResp`), chuyển `cfg_acq_state := s_rel`. Trả về `return` ngay cho CPU dưới dạng non-blocking / optimistic.
  3. Tracker gửi gói tin `mRelease(client_id=n)` qua NoC tới Manager.
  4. Tại Manager: Chuyển trạng thái sang `s_rel_wait`. Chờ cho đến khi toàn bộ hàng đợi lệnh được xử lý hết (`!io.busy(accel) && inst_q.count == 0`).
  5. Sau khi drain sạch, Manager gửi phản hồi `sRelResp` về Client để xác nhận hoàn tất.
  6. Ngay sau đó, Manager chuyển sang trạng thái `s_sfence`: kích hoạt tín hiệu `io.ptw.sfence.valid = true` nhằm **xóa sạch toàn bộ TLB và PTE-cache** thuộc về tenant vừa giải phóng. Cuối cùng chuyển trạng thái về `s_idle`.
  7. Tại Client: Nhận phản hồi `sRelResp`, cập nhật lại `cfg_acq_state := s_idle`.
  * **Lưu ý cực kỳ quan trọng cho kỹ sư thiết kế (Tape-out checklist)**: Lệnh `SFENCE` là cơ chế cách ly bắt buộc giữa các tenant. Nếu một client khác cố gắng acquire lại Manager này trước khi quá trình `sRelResp + sfence` hoàn tất, yêu cầu sẽ bị từ chối tạm thời (Manager state != `s_idle`). Do đó, tầng phần mềm (software driver) **bắt buộc phải có cơ chế retry loop**, không được giả định rằng thao tác release có hiệu lực tức thời ngay lập tức.

---

## 5. Danh Sách Cấu Trúc Dữ Liệu & Giao Tiếp (Structures & Interfaces Reference)

### 5.1. Bảng Đăng Ký CSR (Host CPU Address Map)
```cpp
#define CSR_RROPC0  0x800  // Slot 0 cho decode opcode tùy chỉnh
#define CSR_RROPC3  0x803  // Slot 3 cho decode opcode tùy chỉnh
#define CSR_RRBAR   0x804  // Thanh ghi cơ sở / Kích hoạt Fence / Bar
#define CSR_RRCFG0  0x810  // Cấu hình tenant 0
#define CSR_RRCFG15 0x81F  // Cấu hình tenant 15 (9-bit: bit[8] = acq bit, bit[7:0] = mgr_id)
```

### 5.2. Cấu Trúc Gói Tin NoC (Bundle Định Nghĩa Bằng Chisel/Verilog)
```scala
// Cấu trúc gói tin Request kênh điều khiển và dữ liệu
class RoCCReqBundle extends Bundle {
  val opcode     = UInt(3.W) // 0: mAcquire, 1: mInst, 2: mUStatus, 3: mUPtbr, 4: mRelease, 5: mUnbusy
  val client_id  = UInt(4.W) // Định danh client phát lệnh (0 - 15)
  val manager_id = UInt(8.W)// Định danh đích đến của Manager
  val data       = UInt(64.W)// Dữ liệu tải trọng (instruction encoding, rs1, rs2, status, ptbr)
}

// Cấu trúc gói tin Response phản hồi về từ Accelerator
class RoCCRespBundle extends Bundle {
  val opcode     = UInt(3.W) // 0: sAcqResp, 1: sInstAck, 2: sWrite, 3: sRelResp, 4: sUnbusyAck
  val status     = Bool()    // Cờ trạng thái (vd: grant/deny trong sAcqResp)
  val data       = UInt(64.W)// Dữ liệu trả về hoặc mã thanh ghi đích rd
}
```

### 5.3. Định Nghĩa Trạng Thái FSM Phía Manager (Manager Core States)

```scala
// Định nghĩa các trạng thái FSM bên trong Manager Core (Chisel/Verilog Code Block)
val s_idle     = "b000".U(3.W) // Trạng thái rảnh, chờ nhận yêu cầu mAcquire từ client mới
val s_active   = "b001".U(3.W) // Đang được chiếm giữ bởi một client hợp lệ, sẵn sàng nhận và xử lý lệnh từ inst_q
val s_rel_wait = "b010".U(3.W) // Chờ drain toàn bộ hàng đợi lệnh (inst_q.count == 0 và !io.busy) trước khi trả về thông điệp giải phóng
val s_sfence   = "b011".U(3.W) // Kích hoạt lệnh xóa sạch TLB/PTE-cache (io.ptw.sfence.valid = true) để cô lập dữ liệu giữa các multi-tenant
val s_unbusy   = "b100".U(3.W) // Xử lý yêu cầu rào chắn đồng bộ mUnbusy (Fence)
```
