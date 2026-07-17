#include "accelerators/accel_base.hh"

#include "base/logging.hh"
#include "base/trace.hh"
#include "debug/Accel.hh"
#include "sim/system.hh"

namespace gem5
{

std::unordered_map<int, AccelBase *> AccelBase::registry;

AccelBase::AccelBase(const Params &p) :
    DmaDevice(p),
    accelId(p.accel_id),
    state(State::Idle),
    ctrlReg(0), statusReg(0),
    srcAddrReg(0), dstAddrReg(0), lenReg(0),
    param0Reg(0), param1Reg(0), cyclesLastReg(0),
    opStartTick(0),
    fetchDoneEvent([this]{ onFetchDone(); }, name() + ".fetchDoneEvent"),
    computeDoneEvent([this]{ onComputeDone(); }, name() + ".computeDoneEvent"),
    writebackDoneEvent([this]{ onWritebackDone(); },
                        name() + ".writebackDoneEvent"),
    stats(*this)
{
    panic_if(registry.count(accelId),
             "AccelBase: accel_id %d already registered by another device "
             "(%s clashes with an existing device). Give each accelerator "
             "instance a unique accel_id in the Python config.",
             accelId, name());
    registry[accelId] = this;
}

AccelBase::AccelStats::AccelStats(AccelBase &a) :
    statistics::Group(&a),
    ADD_STAT(opsCompleted, statistics::units::Count::get(),
             "Number of accelerator operations completed"),
    ADD_STAT(opsFailed, statistics::units::Count::get(),
             "Number of operations rejected (e.g. started while busy)"),
    ADD_STAT(bytesTransferred, statistics::units::Byte::get(),
             "Total bytes moved over DMA (read + write) by this device"),
    ADD_STAT(busyCycles, statistics::units::Cycle::get(),
             "Sum of device-cycle compute latency across all completed ops")
{
}

AccelBase *
AccelBase::lookup(int accelId)
{
    auto it = registry.find(accelId);
    return it == registry.end() ? nullptr : it->second;
}

AddrRangeList
AccelBase::getAddrRanges() const
{
    return AddrRangeList({AddrRange(pioAddr, pioAddr + RegWindowSize)});
}

Tick
AccelBase::write(PacketPtr pkt)
{
    panic_if(pkt->getSize() != sizeof(uint64_t),
             "%s: only 8-byte MMIO writes are supported (got %d bytes)",
             accelName(), pkt->getSize());

    const Addr off = pkt->getAddr() - pioAddr;
    const uint64_t val = pkt->getLE<uint64_t>();

    switch (off) {
      case REG_SRC_ADDR: srcAddrReg = val; break;
      case REG_DST_ADDR: dstAddrReg = val; break;
      case REG_LEN:       lenReg    = val; break;
      case REG_PARAM0:    param0Reg = val; break;
      case REG_PARAM1:    param1Reg = val; break;
      case REG_STATUS:
        // Software acknowledges completion/error by writing 0.
        if (val == 0)
            statusReg &= ~(STATUS_DONE | STATUS_ERROR);
        break;
      case REG_CTRL:
        ctrlReg = val;
        if (val & CTRL_START) {
            uint64_t rc = startOp(srcAddrReg, dstAddrReg, lenReg,
                                   param0Reg, param1Reg);
            if (rc != 0)
                DPRINTF(Accel, "%s: MMIO start rejected (busy)\n",
                        accelName());
        }
        break;
      default:
        DPRINTF(Accel, "%s: write to unknown MMIO offset %#x ignored\n",
                accelName(), off);
        break;
    }

    pkt->makeResponse();
    return pioDelay;
}

Tick
AccelBase::read(PacketPtr pkt)
{
    panic_if(pkt->getSize() != sizeof(uint64_t),
             "%s: only 8-byte MMIO reads are supported (got %d bytes)",
             accelName(), pkt->getSize());

    const Addr off = pkt->getAddr() - pioAddr;
    uint64_t val = 0;

    switch (off) {
      case REG_CTRL:        val = ctrlReg; break;
      case REG_STATUS:      val = statusReg; break;
      case REG_SRC_ADDR:    val = srcAddrReg; break;
      case REG_DST_ADDR:    val = dstAddrReg; break;
      case REG_LEN:         val = lenReg; break;
      case REG_PARAM0:      val = param0Reg; break;
      case REG_PARAM1:      val = param1Reg; break;
      case REG_CYCLES_LAST: val = cyclesLastReg; break;
      default:
        DPRINTF(Accel, "%s: read from unknown MMIO offset %#x -> 0\n",
                accelName(), off);
        break;
    }

    pkt->setLE<uint64_t>(val);
    pkt->makeResponse();
    return pioDelay;
}

uint64_t
AccelBase::startOp(Addr srcPaddr, Addr dstPaddr, uint64_t lenBytes,
                    uint64_t param0, uint64_t param1)
{
    if (state != State::Idle || (statusReg & STATUS_BUSY)) {
        statusReg |= STATUS_ERROR;
        stats.opsFailed++;
        DPRINTF(Accel, "%s: startOp rejected, device busy\n", accelName());
        return STATUS_ERROR;
    }
    if (lenBytes == 0) {
        statusReg |= STATUS_ERROR;
        stats.opsFailed++;
        return STATUS_ERROR;
    }

    srcAddrReg = srcPaddr;
    dstAddrReg = dstPaddr;
    lenReg = lenBytes;
    param0Reg = param0;
    param1Reg = param1;

    beginOp();
    return 0;
}

void
AccelBase::beginOp()
{
    state = State::Fetch;
    statusReg = STATUS_BUSY;
    opStartTick = curTick();

    scratch.reset(new uint8_t[lenReg]);
    DPRINTF(Accel, "%s: FETCH src=%#x len=%d\n", accelName(), srcAddrReg,
            lenReg);
    dmaRead(srcAddrReg, static_cast<int>(lenReg), &fetchDoneEvent,
            scratch.get());
}

void
AccelBase::onFetchDone()
{
    state = State::Compute;
    beginCompute();
}

void
AccelBase::beginCompute()
{
    Cycles c = computeLatency(lenReg, param0Reg, param1Reg);
    DPRINTF(Accel, "%s: COMPUTE for %d cycles\n", accelName(), (uint64_t)c);
    schedule(computeDoneEvent, curTick() + cyclesToTicks(c));
}

Cycles
AccelBase::computeLatency(uint64_t, uint64_t, uint64_t) const
{
    panic("%s: computeLatency() was called but not overridden, and "
          "beginCompute() was not overridden either - a subclass must "
          "provide at least one of the two.", name());
    return Cycles(0); // unreachable - panic() aborts, but gem5's panic()
                       // macro isn't recognized as [[noreturn]] by the
                       // compiler (see src/base/logging.hh), so a real
                       // return statement is required here to avoid a
                       // "control reaches end of non-void function"
                       // build error.
}

void
AccelBase::onComputeDone()
{
    state = State::Writeback;
    DPRINTF(Accel, "%s: WRITEBACK dst=%#x len=%d\n", accelName(), dstAddrReg,
            lenReg);
    dmaWrite(dstAddrReg, static_cast<int>(lenReg), &writebackDoneEvent,
             scratch.get());
}

void
AccelBase::onWritebackDone()
{
    Cycles totalCycles = ticksToCycles(curTick() - opStartTick);
    cyclesLastReg = static_cast<uint64_t>(totalCycles);

    statusReg = STATUS_DONE;
    state = State::Idle;
    scratch.reset();

    stats.opsCompleted++;
    stats.bytesTransferred += 2 * lenReg; // read + write
    stats.busyCycles += cyclesLastReg;

    DPRINTF(Accel, "%s: DONE in %d cycles\n", accelName(), cyclesLastReg);
}

} // namespace gem5
