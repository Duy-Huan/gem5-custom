#include "npu/npu_csr_registry.hh"
#include "debug/NpuSimObject.hh"

namespace gem5 {

void CsrCallbackRegistry::registerCsr(const CsrDescriptor &desc) {
    if (registry.find(desc.offset) != registry.end()) {
        warn("NPU CSR: Overwriting existing CSR at offset %#x\n", desc.offset);
    }
    registry[desc.offset] = desc;
    DPRINTF(NpuSimObject, "CSR registered: '%s' at offset=%#x\n", desc.name.c_str(), desc.offset);
}

void CsrCallbackRegistry::unregisterCsr(Addr offset) {
    auto it = registry.find(offset);
    if (it != registry.end()) registry.erase(it);
}

uint32_t CsrCallbackRegistry::read(Addr offset) const {
    auto it = registry.find(offset);
    if (it == registry.end()) return 0;
    const CsrDescriptor &desc = it->second;
    if (desc.access == CsrAccessType::WRITE) return 0;
    return desc.onRead ? desc.onRead() : desc.resetValue;
}

void CsrCallbackRegistry::write(Addr offset, uint32_t value) {
    auto it = registry.find(offset);
    if (it == registry.end()) return;
    const CsrDescriptor &desc = it->second;
    if (desc.access == CsrAccessType::READ) return;
    uint32_t oldVal = desc.onRead ? desc.onRead() : desc.resetValue;
    uint32_t newVal = applyWriteBehavior(oldVal, value, desc);
    if (desc.onWrite) desc.onWrite(newVal);
}

bool CsrCallbackRegistry::hasCsr(Addr offset) const {
    return registry.find(offset) != registry.end();
}

const CsrDescriptor* CsrCallbackRegistry::getDescriptor(Addr offset) const {
    auto it = registry.find(offset);
    return (it != registry.end()) ? &(it->second) : nullptr;
}

void CsrCallbackRegistry::resetAll() {
    for (auto &pair : registry) {
        if (pair.second.onWrite) pair.second.onWrite(pair.second.resetValue);
    }
}

std::vector<Addr> CsrCallbackRegistry::getAllOffsets() const {
    std::vector<Addr> offsets;
    for (const auto &pair : registry) offsets.push_back(pair.first);
    return offsets;
}

void CsrCallbackRegistry::dump() const {
    DPRINTF(NpuSimObject, "CSR Registry: %zu entries\n", registry.size());
}

uint32_t CsrCallbackRegistry::applyWriteBehavior(uint32_t oldVal, uint32_t newVal,
                                                   const CsrDescriptor &desc) const {
    if (desc.writeClear) return oldVal & ~newVal;
    if (desc.writeSet) return oldVal | newVal;
    return newVal;
}

} // namespace gem5
