#ifndef __NPU_NPU_SIM_OBJECT_HH__
#define __NPU_NPU_SIM_OBJECT_HH__

#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <string>
#include <vector>
#include "base/statistics.hh"
#include "mem/port.hh"
#include "npu/npu_csr_registry.hh"
#include "params/NpuSimObject.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5 {

extern "C" { extern const char* NpuSimObjectDebugFlag; }

enum NpuCsrOffset : Addr {
    REG_CTRL=0x00, REG_STATUS=0x04, REG_WEIGHT_ADDR=0x08, REG_WEIGHT_ADDR_HI=0x0C,
    REG_INSTR_ADDR=0x10, REG_INSTR_ADDR_HI=0x14, REG_FEATURE_SRC=0x18,
    REG_FEATURE_SRC_HI=0x1C, REG_FEATURE_DST=0x20, REG_FEATURE_DST_HI=0x24,
    REG_INT_CLEAR=0x28, REG_VERSION=0x2C, REG_PE_CONFIG=0x30, REG_DMA_STATUS=0x34,
    REG_CYCLE_COUNT_LO=0x38, REG_CYCLE_COUNT_HI=0x3C, CSR_REGION_SIZE=0x1000
};

constexpr uint32_t CTRL_START_BIT=(1U<<0), CTRL_RESET_BIT=(1U<<1), CTRL_INT_EN_BIT=(1U<<2);
constexpr uint32_t STATUS_IDLE_BIT=(1U<<0), STATUS_BUSY_BIT=(1U<<1), STATUS_DONE_BIT=(1U<<2);
constexpr uint32_t STATUS_ERROR_BIT=(1U<<3), STATUS_WEIGHT_LOADED_BIT=(1U<<4);

enum NpuOpcode : uint32_t {
    OP_NOP=0x00, OP_LOAD_W=0x01, OP_LOAD_F=0x02, OP_STORE_F=0x03,
    OP_CONV2D=0x10, OP_DEPTHWISE=0x11, OP_MATMUL=0x12,
    OP_ADD=0x20, OP_RELU=0x21, OP_SILU=0x22,
    OP_MAXPOOL=0x30, OP_AVGPOOL=0x31, OP_END=0xFF
};

struct __attribute__((packed)) NpuInstruction {
    uint32_t opcode, param0, param1, param2, param3, param4, param5, param6,
             param7, param8, param9, param10, param11, param12, param13, param14, param15;
};
static_assert(sizeof(NpuInstruction)==64, "NpuInstruction must be 64 bytes");

class NpuSimObject;

class NpuCpuPort : public ResponsePort {
  private: NpuSimObject *owner;
  public:
    NpuCpuPort(const std::string& name, NpuSimObject *owner_)
        : ResponsePort(name, owner_), owner(owner_) {}
    AddrRangeList getAddrRanges() const override;
  protected:
    Tick recvAtomic(PacketPtr pkt) override;
    void recvFunctional(PacketPtr pkt) override;
    bool recvTimingReq(PacketPtr pkt) override;
    void recvRespRetry() override;
};

class NpuMemPort : public RequestPort {
  private: NpuSimObject *owner; PacketPtr blockedPacket;
  public:
    NpuMemPort(const std::string& name, NpuSimObject *owner_)
        : RequestPort(name, owner_), owner(owner_), blockedPacket(nullptr) {}
    void sendPacket(PacketPtr pkt);
    bool isBlocked() const { return blockedPacket != nullptr; }
  protected:
    bool recvTimingResp(PacketPtr pkt) override;
    void recvReqRetry() override;
    void recvRangeChange() override;
};

class NpuSimObject : public ClockedObject {
  public:
    typedef NpuSimObjectParams Params;
    NpuSimObject(const Params &params);
    ~NpuSimObject();
    Port &getPort(const std::string &if_name, PortID idx=InvalidPortID) override;
    Tick handleAtomic(PacketPtr pkt);
    void handleFunctional(PacketPtr pkt);
    bool handleTimingReq(PacketPtr pkt);
    bool handleMemResp(PacketPtr pkt);
    void handleReqRetry();
    void dmaRead(Addr addr, uint32_t size, uint8_t *dst);
    void dmaWrite(Addr addr, uint32_t size, const uint8_t *src);
    void startComputation();
    void executeInstruction();
    void finishComputation();
    void raiseInterrupt();
    void clearInterrupt();
    void backdoorLoadWeights();
    void regStats() override;

    uint32_t getRegCtrl() const { return regCtrl; }
    uint32_t getRegStatus() const { return regStatus; }
    uint32_t getRegWeightAddrLo() const { return regWeightAddr & 0xFFFFFFFF; }
    uint32_t getRegWeightAddrHi() const { return (regWeightAddr >> 32) & 0xFFFFFFFF; }
    uint32_t getRegInstrAddrLo() const { return regInstrAddr & 0xFFFFFFFF; }
    uint32_t getRegInstrAddrHi() const { return (regInstrAddr >> 32) & 0xFFFFFFFF; }
    uint32_t getRegFeatureSrcLo() const { return regFeatureSrc & 0xFFFFFFFF; }
    uint32_t getRegFeatureSrcHi() const { return (regFeatureSrc >> 32) & 0xFFFFFFFF; }
    uint32_t getRegFeatureDstLo() const { return regFeatureDst & 0xFFFFFFFF; }
    uint32_t getRegFeatureDstHi() const { return (regFeatureDst >> 32) & 0xFFFFFFFF; }
    uint32_t getRegVersion() const { return 0x00010000; }
    uint32_t getRegPeConfig() const { return (numPeLanes << 16) | busWidthBytes; }
    uint32_t getRegDmaStatus() const;
    uint32_t getRegCycleCountLo() const;
    uint32_t getRegCycleCountHi() const;

    void setRegCtrl(uint32_t value);
    void setRegStatus(uint32_t value);
    void setRegWeightAddrLo(uint32_t value);
    void setRegWeightAddrHi(uint32_t value);
    void setRegInstrAddrLo(uint32_t value);
    void setRegInstrAddrHi(uint32_t value);
    void setRegFeatureSrcLo(uint32_t value);
    void setRegFeatureSrcHi(uint32_t value);
    void setRegFeatureDstLo(uint32_t value);
    void setRegFeatureDstHi(uint32_t value);
    void setRegIntClear(uint32_t value);
    void setRegPeConfig(uint32_t value);
    AddrRangeList getAddrRanges() const;

  private:
    NpuCpuPort cpuPort;
    NpuMemPort memPort;
    CsrCallbackRegistry csrRegistry;
    const uint32_t numPeLanes, sramSizeBytes, busWidthBytes;
    const Addr pioAddr, pioSize;
    const std::string weightFilePath;
    uint32_t regCtrl, regStatus, regPeConfig;
    uint64_t regWeightAddr, regInstrAddr, regFeatureSrc, regFeatureDst, cycleCount;
    std::vector<uint8_t> sram;
    enum DmaState { DMA_IDLE, DMA_READ_PENDING, DMA_WRITE_PENDING };
    DmaState dmaState;
    uint8_t *dmaBuffer;
    uint32_t dmaRemaining, dmaOffset;
    bool dmaPending;
    enum NpuState { NPU_IDLE, NPU_FETCH_INSTR, NPU_EXECUTING, NPU_DMA_WAIT, NPU_DONE };
    NpuState npuState;
    uint64_t pc;
    uint32_t instrCount, currentInstrIdx;
    std::vector<NpuInstruction> instructionCache;
    EventFunctionWrapper computeEvent, dmaCompleteEvent;
    bool cpuPortBlocked, interruptPending;
    Tick npuStartTick;

    struct NpuStats : public statistics::Group {
        NpuStats(statistics::Group *parent);
        statistics::Scalar totalOperations, dmaReadBytes, dmaWriteBytes,
                           activeCycles, idleCycles, interruptCount,
                           instrCount, totalLatency;
    } stats;

    void initCsrRegistry();
    void scheduleComputeEvent(Tick delay);
    void scheduleDmaCompleteEvent(Tick delay);
    void processComputeEvent();
    void processDmaCompleteEvent();
    bool isCsrAccess(Addr addr) const;
    Addr csrOffset(Addr addr) const;
    uint32_t calculateLatency(const NpuInstruction &instr);
    void performMacOperation(const NpuInstruction &instr);
    void performLoadWeight(const NpuInstruction &instr);
    void performLoadFeature(const NpuInstruction &instr);
    void performStoreFeature(const NpuInstruction &instr);
    void sendDmaRequest(Addr addr, uint32_t size, bool isWrite, uint8_t *buffer);
};

} // namespace gem5
#endif
