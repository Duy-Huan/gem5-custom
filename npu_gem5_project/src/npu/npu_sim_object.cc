#include "npu/npu_sim_object.hh"
#include <algorithm>
#include <cstring>
#include "base/trace.hh"
#include "debug/NpuSimObject.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/system.hh"

namespace gem5 {

const char* NpuSimObjectDebugFlag = "NpuSimObject";

NpuSimObject::NpuStats::NpuStats(statistics::Group *parent)
    : statistics::Group(parent, "npu"),
      totalOperations(this, "total_operations", "Total MAC/ALU ops"),
      dmaReadBytes(this, "dma_read_bytes", "Bytes read via DMA"),
      dmaWriteBytes(this, "dma_write_bytes", "Bytes written via DMA"),
      activeCycles(this, "active_cycles", "Active compute cycles"),
      idleCycles(this, "idle_cycles", "Idle cycles"),
      interruptCount(this, "interrupt_count", "Interrupts raised"),
      instrCount(this, "instruction_count", "Instructions executed"),
      totalLatency(this, "total_latency_ticks", "Total latency in ticks") {}

AddrRangeList NpuSimObject::NpuCpuPort::getAddrRanges() const {
    return owner->getAddrRanges();
}
Tick NpuSimObject::NpuCpuPort::recvAtomic(PacketPtr pkt) {
    return owner->handleAtomic(pkt);
}
void NpuSimObject::NpuCpuPort::recvFunctional(PacketPtr pkt) {
    owner->handleFunctional(pkt);
}
bool NpuSimObject::NpuCpuPort::recvTimingReq(PacketPtr pkt) {
    return owner->handleTimingReq(pkt);
}
void NpuSimObject::NpuCpuPort::recvRespRetry() {
    DPRINTF(NpuSimObject, "NpuCpuPort::recvRespRetry\n");
}

void NpuSimObject::NpuMemPort::sendPacket(PacketPtr pkt) {
    panic_if(blockedPacket != nullptr, "NpuMemPort: blocked!");
    if (!sendTimingReq(pkt)) blockedPacket = pkt;
}
bool NpuSimObject::NpuMemPort::recvTimingResp(PacketPtr pkt) {
    return owner->handleMemResp(pkt);
}
void NpuSimObject::NpuMemPort::recvReqRetry() {
    assert(blockedPacket != nullptr);
    PacketPtr pkt = blockedPacket;
    blockedPacket = nullptr;
    sendPacket(pkt);
}
void NpuSimObject::NpuMemPort::recvRangeChange() {
    owner->cpuPort.sendRangeChange();
}

uint32_t NpuSimObject::getRegDmaStatus() const {
    uint32_t s = 0;
    if (dmaState == DMA_IDLE) s |= (1U<<0);
    if (dmaState == DMA_READ_PENDING) s |= (1U<<1);
    if (dmaState == DMA_WRITE_PENDING) s |= (1U<<2);
    if (dmaPending) s |= (1U<<3);
    return s;
}
uint32_t NpuSimObject::getRegCycleCountLo() const { return cycleCount & 0xFFFFFFFF; }
uint32_t NpuSimObject::getRegCycleCountHi() const { return (cycleCount >> 32) & 0xFFFFFFFF; }

void NpuSimObject::setRegCtrl(uint32_t value) {
    regCtrl = value;
    if (value & CTRL_START_BIT) {
        regCtrl &= ~CTRL_START_BIT;
        startComputation();
    }
    if (value & CTRL_RESET_BIT) {
        regCtrl = 0; regStatus = STATUS_IDLE_BIT; npuState = NPU_IDLE;
        pc = 0; currentInstrIdx = 0; instructionCache.clear();
        interruptPending = false; cycleCount = 0;
    }
}
void NpuSimObject::setRegStatus(uint32_t value) {
    if (value == 0) regStatus &= ~STATUS_DONE_BIT;
}
void NpuSimObject::setRegWeightAddrLo(uint32_t value) {
    regWeightAddr = (regWeightAddr & ~0xFFFFFFFFULL) | value;
}
void NpuSimObject::setRegWeightAddrHi(uint32_t value) {
    regWeightAddr = (regWeightAddr & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
}
void NpuSimObject::setRegInstrAddrLo(uint32_t value) {
    regInstrAddr = (regInstrAddr & ~0xFFFFFFFFULL) | value;
}
void NpuSimObject::setRegInstrAddrHi(uint32_t value) {
    regInstrAddr = (regInstrAddr & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
}
void NpuSimObject::setRegFeatureSrcLo(uint32_t value) {
    regFeatureSrc = (regFeatureSrc & ~0xFFFFFFFFULL) | value;
}
void NpuSimObject::setRegFeatureSrcHi(uint32_t value) {
    regFeatureSrc = (regFeatureSrc & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
}
void NpuSimObject::setRegFeatureDstLo(uint32_t value) {
    regFeatureDst = (regFeatureDst & ~0xFFFFFFFFULL) | value;
}
void NpuSimObject::setRegFeatureDstHi(uint32_t value) {
    regFeatureDst = (regFeatureDst & 0xFFFFFFFFULL) | ((uint64_t)value << 32);
}
void NpuSimObject::setRegIntClear(uint32_t value) {
    if (value & 0x1) { clearInterrupt(); regStatus &= ~STATUS_DONE_BIT; }
}
void NpuSimObject::setRegPeConfig(uint32_t value) {}

void NpuSimObject::initCsrRegistry() {
    auto reg = [&](const char* name, Addr off, uint32_t sz, CsrAccessType acc,
                   auto rd, auto wr, uint32_t rst, bool wclr=false, bool wset=false) {
        CsrDescriptor d; d.name=name; d.offset=off; d.size=sz; d.access=acc;
        d.onRead=rd; d.onWrite=wr; d.resetValue=rst; d.writeClear=wclr; d.writeSet=wset;
        csrRegistry.registerCsr(d);
    };
    reg("REG_CTRL", REG_CTRL, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegCtrl();},
        [this](uint32_t v){setRegCtrl(v);}, 0);
    reg("REG_STATUS", REG_STATUS, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegStatus();},
        [this](uint32_t v){setRegStatus(v);}, STATUS_IDLE_BIT, true);
    reg("REG_WEIGHT_ADDR_LO", REG_WEIGHT_ADDR, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegWeightAddrLo();},
        [this](uint32_t v){setRegWeightAddrLo(v);}, 0);
    reg("REG_WEIGHT_ADDR_HI", REG_WEIGHT_ADDR_HI, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegWeightAddrHi();},
        [this](uint32_t v){setRegWeightAddrHi(v);}, 0);
    reg("REG_INSTR_ADDR_LO", REG_INSTR_ADDR, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegInstrAddrLo();},
        [this](uint32_t v){setRegInstrAddrLo(v);}, 0);
    reg("REG_INSTR_ADDR_HI", REG_INSTR_ADDR_HI, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegInstrAddrHi();},
        [this](uint32_t v){setRegInstrAddrHi(v);}, 0);
    reg("REG_FEATURE_SRC_LO", REG_FEATURE_SRC, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegFeatureSrcLo();},
        [this](uint32_t v){setRegFeatureSrcLo(v);}, 0);
    reg("REG_FEATURE_SRC_HI", REG_FEATURE_SRC_HI, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegFeatureSrcHi();},
        [this](uint32_t v){setRegFeatureSrcHi(v);}, 0);
    reg("REG_FEATURE_DST_LO", REG_FEATURE_DST, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegFeatureDstLo();},
        [this](uint32_t v){setRegFeatureDstLo(v);}, 0);
    reg("REG_FEATURE_DST_HI", REG_FEATURE_DST_HI, 4, CsrAccessType::READ_WRITE,
        [this]()->uint32_t{return getRegFeatureDstHi();},
        [this](uint32_t v){setRegFeatureDstHi(v);}, 0);
    reg("REG_INT_CLEAR", REG_INT_CLEAR, 4, CsrAccessType::WRITE,
        nullptr, [this](uint32_t v){setRegIntClear(v);}, 0);
    reg("REG_VERSION", REG_VERSION, 4, CsrAccessType::READ,
        [this]()->uint32_t{return getRegVersion();}, nullptr, 0x00010000);
    reg("REG_PE_CONFIG", REG_PE_CONFIG, 4, CsrAccessType::READ,
        [this]()->uint32_t{return getRegPeConfig();}, nullptr, (numPeLanes<<16)|busWidthBytes);
    reg("REG_DMA_STATUS", REG_DMA_STATUS, 4, CsrAccessType::READ,
        [this]()->uint32_t{return getRegDmaStatus();}, nullptr, 0);
    reg("REG_CYCLE_COUNT_LO", REG_CYCLE_COUNT_LO, 4, CsrAccessType::READ,
        [this]()->uint32_t{return getRegCycleCountLo();}, nullptr, 0);
    reg("REG_CYCLE_COUNT_HI", REG_CYCLE_COUNT_HI, 4, CsrAccessType::READ,
        [this]()->uint32_t{return getRegCycleCountHi();}, nullptr, 0);
}

NpuSimObject::NpuSimObject(const Params &params)
    : ClockedObject(params),
      cpuPort(name() + ".cpu_port", this),
      memPort(name() + ".mem_port", this),
      numPeLanes(params.num_pe_lanes),
      sramSizeBytes(params.sram_size_kb * 1024),
      busWidthBytes(params.bus_width_bytes),
      pioAddr(params.pio_addr), pioSize(params.pio_size),
      weightFilePath(params.weight_file_path),
      regCtrl(0), regStatus(STATUS_IDLE_BIT), regPeConfig((numPeLanes<<16)|busWidthBytes),
      cycleCount(0), sram(sramSizeBytes, 0), dmaState(DMA_IDLE),
      dmaBuffer(nullptr), dmaRemaining(0), dmaOffset(0), dmaPending(false),
      npuState(NPU_IDLE), pc(0), instrCount(0), currentInstrIdx(0),
      computeEvent([this]{processComputeEvent();}, name()+".compute"),
      dmaCompleteEvent([this]{processDmaCompleteEvent();}, name()+".dma_complete"),
      cpuPortBlocked(false), interruptPending(false), npuStartTick(0), stats(this) {
    initCsrRegistry();
    if (!weightFilePath.empty()) backdoorLoadWeights();
}
NpuSimObject::~NpuSimObject() {}

Port &NpuSimObject::getPort(const std::string &if_name, PortID idx) {
    if (if_name == "cpu_port") return cpuPort;
    if (if_name == "mem_port") return memPort;
    return ClockedObject::getPort(if_name, idx);
}
AddrRangeList NpuSimObject::getAddrRanges() const {
    return AddrRangeList{AddrRange(pioAddr, pioAddr + pioSize - 1)};
}

Tick NpuSimObject::handleAtomic(PacketPtr pkt) {
    Addr off = csrOffset(pkt->getAddr());
    if (pkt->isRead()) pkt->setLE<uint32_t>(csrRegistry.read(off));
    else if (pkt->isWrite()) csrRegistry.write(off, pkt->getLE<uint32_t>());
    return 0;
}
void NpuSimObject::handleFunctional(PacketPtr pkt) {
    Addr off = csrOffset(pkt->getAddr());
    if (pkt->isRead()) pkt->setLE<uint32_t>(csrRegistry.read(off));
    else if (pkt->isWrite()) csrRegistry.write(off, pkt->getLE<uint32_t>());
}
bool NpuSimObject::handleTimingReq(PacketPtr pkt) {
    Addr addr = pkt->getAddr();
    if (!isCsrAccess(addr)) { pkt->makeResponse(); cpuPort.sendTimingResp(pkt); return true; }
    if (cpuPortBlocked) return false;
    cpuPortBlocked = true;
    Addr off = csrOffset(addr);
    if (pkt->isRead()) pkt->setLE<uint32_t>(csrRegistry.read(off));
    else if (pkt->isWrite()) csrRegistry.write(off, pkt->getLE<uint32_t>());
    pkt->makeResponse();
    schedule(new EventFunctionWrapper([this, pkt]() {
        cpuPort.sendTimingResp(pkt);
        cpuPortBlocked = false;
    }, name() + ".mmio_resp", true), clockEdge(Cycles(1)));
    return true;
}

bool NpuSimObject::handleMemResp(PacketPtr pkt) {
    assert(dmaState != DMA_IDLE && dmaBuffer != nullptr);
    if (pkt->isError()) {
        regStatus |= STATUS_ERROR_BIT; npuState = NPU_DONE; dmaState = DMA_IDLE;
        finishComputation(); pkt->destroy(); return true;
    }
    if (pkt->isRead()) {
        uint32_t cs = std::min((uint32_t)pkt->getSize(), dmaRemaining);
        std::memcpy(dmaBuffer + dmaOffset, pkt->getPtr<uint8_t>(), cs);
        dmaOffset += cs; dmaRemaining -= cs; stats.dmaReadBytes += cs;
    } else if (pkt->isWrite()) {
        dmaRemaining -= pkt->getSize(); stats.dmaWriteBytes += pkt->getSize();
    }
    pkt->destroy();
    if (dmaRemaining == 0) {
        dmaState = DMA_IDLE; dmaPending = false;
        schedule(dmaCompleteEvent, clockEdge(Cycles(1)));
    }
    return true;
}
void NpuSimObject::handleReqRetry() { memPort.recvReqRetry(); }

void NpuSimObject::sendDmaRequest(Addr addr, uint32_t size, bool isWrite, uint8_t *buffer) {
    assert(dmaState == DMA_IDLE && !dmaPending);
    dmaBuffer = buffer; dmaRemaining = size; dmaOffset = 0; dmaPending = true;
    uint32_t chunkSize = busWidthBytes;
    uint32_t numChunks = (size + chunkSize - 1) / chunkSize;
    for (uint32_t i = 0; i < numChunks; ++i) {
        uint32_t tc = std::min(chunkSize, size - i * chunkSize);
        Addr ca = addr + i * chunkSize;
        auto req = std::make_shared<Request>(ca, tc, Request::Flags(), 0);
        PacketPtr pkt;
        if (isWrite) { pkt = new Packet(req, MemCmd::WriteReq); pkt->dataStatic(buffer + i * chunkSize); }
        else { pkt = new Packet(req, MemCmd::ReadReq); pkt->allocate(); }
        memPort.sendPacket(pkt);
        if (isWrite) { dmaRemaining -= tc; stats.dmaWriteBytes += tc; }
    }
    if (isWrite) { dmaState = DMA_IDLE; dmaPending = false; schedule(dmaCompleteEvent, clockEdge(Cycles(numChunks))); }
    else dmaState = DMA_READ_PENDING;
}
void NpuSimObject::dmaRead(Addr addr, uint32_t size, uint8_t *dst) {
    sendDmaRequest(addr, size, false, dst);
}
void NpuSimObject::dmaWrite(Addr addr, uint32_t size, const uint8_t *src) {
    uint8_t *tmp = new uint8_t[size]; std::memcpy(tmp, src, size);
    sendDmaRequest(addr, size, true, tmp);
}

void NpuSimObject::backdoorLoadWeights() {
    std::ifstream file(weightFilePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) { warn("NPU: Cannot open weight file '%s'\n", weightFilePath); return; }
    std::streamsize fs = file.tellg(); file.seekg(0, std::ios::beg);
    uint32_t ls = std::min((uint32_t)fs, sramSizeBytes);
    if (!file.read(reinterpret_cast<char*>(sram.data()), ls)) {
        warn("NPU: Failed to read weight file '%s'\n", weightFilePath);
        file.close(); return;
    }
    file.close(); regStatus |= STATUS_WEIGHT_LOADED_BIT;
}

void NpuSimObject::startComputation() {
    if (npuState != NPU_IDLE) return;
    regStatus = STATUS_BUSY_BIT; npuState = NPU_FETCH_INSTR;
    pc = regInstrAddr; currentInstrIdx = 0; instructionCache.clear(); npuStartTick = curTick();
    uint32_t mib = 64 * 64; uint8_t *ib = new uint8_t[mib]; std::memset(ib, 0, mib);
    dmaRead(regInstrAddr, mib, ib);
}
void NpuSimObject::executeInstruction() {
    if (currentInstrIdx >= instructionCache.size()) { npuState = NPU_DONE; finishComputation(); return; }
    const NpuInstruction &instr = instructionCache[currentInstrIdx];
    stats.instrCount++;
    uint32_t latency = calculateLatency(instr);
    switch (instr.opcode) {
        case OP_NOP: break;
        case OP_LOAD_W: performLoadWeight(instr); return;
        case OP_LOAD_F: performLoadFeature(instr); return;
        case OP_STORE_F: performStoreFeature(instr); return;
        case OP_CONV2D: case OP_DEPTHWISE: case OP_MATMUL: performMacOperation(instr); break;
        case OP_ADD: stats.totalOperations += (uint64_t)instr.param0 * instr.param1; break;
        case OP_RELU: case OP_SILU: stats.totalOperations += instr.param0; break;
        case OP_MAXPOOL: case OP_AVGPOOL: stats.totalOperations += (uint64_t)instr.param0 * instr.param1; break;
        case OP_END: npuState = NPU_DONE; finishComputation(); return;
        default: break;
    }
    currentInstrIdx++; scheduleComputeEvent(latency);
}
uint32_t NpuSimObject::calculateLatency(const NpuInstruction &instr) {
    uint32_t bl = 1;
    switch (instr.opcode) {
        case OP_CONV2D: {
            uint64_t ops = (uint64_t)instr.param2 * instr.param2 * instr.param0 * instr.param1 * instr.param3 * instr.param4;
            bl = (uint32_t)((ops + numPeLanes - 1) / numPeLanes); bl = std::max(bl, 10U); break;
        }
        case OP_DEPTHWISE: {
            uint64_t ops = (uint64_t)instr.param2 * instr.param2 * instr.param0 * instr.param3 * instr.param4;
            bl = (uint32_t)((ops + numPeLanes - 1) / numPeLanes); bl = std::max(bl, 5U); break;
        }
        case OP_MATMUL: {
            uint64_t ops = (uint64_t)instr.param0 * instr.param1 * instr.param2;
            bl = (uint32_t)((ops + numPeLanes - 1) / numPeLanes); bl = std::max(bl, 10U); break;
        }
        case OP_ADD: case OP_RELU: case OP_SILU: bl = 2; break;
        case OP_MAXPOOL: case OP_AVGPOOL: bl = 5; break;
        default: bl = 1; break;
    }
    return bl + 2;
}
void NpuSimObject::performMacOperation(const NpuInstruction &instr) {
    uint64_t ops = 0;
    if (instr.opcode == OP_CONV2D) ops = (uint64_t)instr.param2 * instr.param2 * instr.param0 * instr.param1 * instr.param3 * instr.param4;
    else if (instr.opcode == OP_DEPTHWISE) ops = (uint64_t)instr.param2 * instr.param2 * instr.param0 * instr.param3 * instr.param4;
    else if (instr.opcode == OP_MATMUL) ops = (uint64_t)instr.param0 * instr.param1 * instr.param2;
    stats.totalOperations += ops;
}
void NpuSimObject::performLoadWeight(const NpuInstruction &instr) {
    uint64_t da = ((uint64_t)instr.param9 << 32) | instr.param8;
    uint32_t so = instr.param6, sz = instr.param0;
    if (so + sz > sramSizeBytes) { regStatus |= STATUS_ERROR_BIT; npuState = NPU_DONE; finishComputation(); return; }
    npuState = NPU_DMA_WAIT; dmaRead(da, sz, sram.data() + so);
}
void NpuSimObject::performLoadFeature(const NpuInstruction &instr) {
    uint64_t da = ((uint64_t)instr.param9 << 32) | instr.param8;
    uint32_t so = instr.param6, sz = instr.param0;
    if (so + sz > sramSizeBytes) { regStatus |= STATUS_ERROR_BIT; npuState = NPU_DONE; finishComputation(); return; }
    npuState = NPU_DMA_WAIT; dmaRead(da, sz, sram.data() + so);
}
void NpuSimObject::performStoreFeature(const NpuInstruction &instr) {
    uint64_t da = ((uint64_t)instr.param11 << 32) | instr.param10;
    uint32_t so = instr.param6, sz = instr.param0;
    if (so + sz > sramSizeBytes) { regStatus |= STATUS_ERROR_BIT; npuState = NPU_DONE; finishComputation(); return; }
    npuState = NPU_DMA_WAIT; dmaWrite(da, sz, sram.data() + so);
}
void NpuSimObject::finishComputation() {
    regStatus &= ~STATUS_BUSY_BIT; regStatus |= STATUS_DONE_BIT; npuState = NPU_IDLE;
    if (npuStartTick > 0) stats.totalLatency += curTick() - npuStartTick;
    if (regCtrl & CTRL_INT_EN_BIT) raiseInterrupt();
}
void NpuSimObject::raiseInterrupt() { interruptPending = true; stats.interruptCount++; }
void NpuSimObject::clearInterrupt() { interruptPending = false; }
void NpuSimObject::scheduleComputeEvent(Tick delay) { schedule(computeEvent, clockEdge(Cycles(delay))); }
void NpuSimObject::scheduleDmaCompleteEvent(Tick delay) { schedule(dmaCompleteEvent, clockEdge(Cycles(delay))); }
void NpuSimObject::processComputeEvent() {
    stats.activeCycles++; cycleCount++;
    if (npuState == NPU_FETCH_INSTR) {
        npuState = NPU_EXECUTING; currentInstrIdx = 0;
        NpuInstruction ei; std::memset(&ei, 0, sizeof(ei)); ei.opcode = OP_END;
        instructionCache.push_back(ei); executeInstruction();
    } else if (npuState == NPU_EXECUTING) executeInstruction();
}
void NpuSimObject::processDmaCompleteEvent() {
    if (npuState == NPU_DMA_WAIT) { npuState = NPU_EXECUTING; currentInstrIdx++; executeInstruction(); }
}
void NpuSimObject::regStats() { ClockedObject::regStats(); }

} // namespace gem5
