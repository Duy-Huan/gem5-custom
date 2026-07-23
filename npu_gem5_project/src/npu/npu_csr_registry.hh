#ifndef __NPU_NPU_CSR_REGISTRY_HH__
#define __NPU_NPU_CSR_REGISTRY_HH__

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "base/trace.hh"

namespace gem5 {

enum class CsrAccessType : uint8_t { READ, WRITE, READ_WRITE };
using CsrReadCallback  = std::function<uint32_t(void)>;
using CsrWriteCallback = std::function<void(uint32_t value)>;

struct CsrDescriptor {
    std::string name;
    Addr offset;
    uint32_t size;
    CsrAccessType access;
    CsrReadCallback onRead;
    CsrWriteCallback onWrite;
    uint32_t resetValue;
    bool writeClear;
    bool writeSet;
    bool sticky;
    CsrDescriptor() : name("reserved"), offset(0), size(4),
        access(CsrAccessType::READ_WRITE), onRead(nullptr), onWrite(nullptr),
        resetValue(0), writeClear(false), writeSet(false), sticky(false) {}
};

class CsrCallbackRegistry {
  public:
    CsrCallbackRegistry() = default;
    ~CsrCallbackRegistry() = default;
    void registerCsr(const CsrDescriptor &desc);
    void unregisterCsr(Addr offset);
    uint32_t read(Addr offset) const;
    void write(Addr offset, uint32_t value);
    bool hasCsr(Addr offset) const;
    const CsrDescriptor* getDescriptor(Addr offset) const;
    void resetAll();
    std::vector<Addr> getAllOffsets() const;
    void dump() const;
  private:
    std::map<Addr, CsrDescriptor> registry;
    uint32_t applyWriteBehavior(uint32_t oldVal, uint32_t newVal,
                                 const CsrDescriptor &desc) const;
};

} // namespace gem5
#endif
