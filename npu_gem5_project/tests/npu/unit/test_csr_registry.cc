#include <gtest/gtest.h>
#include "npu/npu_csr_registry.hh"
using namespace gem5;

TEST(CsrCallbackRegistry, TC_1_1_RegisterCsr_Basic) {
    CsrCallbackRegistry reg;
    CsrDescriptor d; d.name="TEST"; d.offset=0x00; d.size=4;
    d.access=CsrAccessType::READ_WRITE;
    d.onRead=[]()->uint32_t{return 0xDEADBEEF;};
    d.onWrite=[](uint32_t v){};
    EXPECT_NO_THROW(reg.registerCsr(d));
    EXPECT_TRUE(reg.hasCsr(0x00));
}

TEST(CsrCallbackRegistry, TC_1_3_Read_Callback) {
    CsrCallbackRegistry reg;
    CsrDescriptor d; d.offset=0x00; d.access=CsrAccessType::READ_WRITE;
    d.onRead=[]()->uint32_t{return 0xAABBCCDD;};
    d.onWrite=[](uint32_t v){};
    reg.registerCsr(d);
    EXPECT_EQ(reg.read(0x00), 0xAABBCCDD);
}

TEST(CsrCallbackRegistry, TC_1_4_Write_Callback) {
    CsrCallbackRegistry reg;
    uint32_t written=0;
    CsrDescriptor d; d.offset=0x00; d.access=CsrAccessType::READ_WRITE;
    d.onRead=[&written]()->uint32_t{return written;};
    d.onWrite=[&written](uint32_t v){written=v;};
    reg.registerCsr(d);
    reg.write(0x00, 0x12345678);
    EXPECT_EQ(written, 0x12345678);
}

TEST(CsrCallbackRegistry, TC_1_7_WriteClear) {
    CsrCallbackRegistry reg;
    uint32_t val=0xFFFFFFFF;
    CsrDescriptor d; d.offset=0x00; d.writeClear=true;
    d.onRead=[&val]()->uint32_t{return val;};
    d.onWrite=[&val](uint32_t v){val=v;};
    reg.registerCsr(d);
    reg.write(0x00, 0x0000000F);
    EXPECT_EQ(val, 0xFFFFFFF0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
