#include <gtest/gtest.h>

class MockNpu {
public:
    enum State { IDLE, FETCH_INSTR, EXECUTING, DMA_WAIT, DONE };
    State state=IDLE;
    uint32_t ctrl=0, status=0x1;
    bool irq=false;
    static constexpr uint32_t START=0x1, RESET=0x2, INT_EN=0x4;
    static constexpr uint32_t IDLE_B=0x1, BUSY_B=0x2, DONE_B=0x4;

    void writeCtrl(uint32_t v) {
        if (v & RESET) { state=IDLE; ctrl=0; status=IDLE_B; irq=false; return; }
        if (v & START) { if (state!=IDLE) return; state=FETCH_INSTR; status=BUSY_B; ctrl&=~START; }
    }
    void finish() { state=DONE; status&=~BUSY_B; status|=DONE_B; if (ctrl&INT_EN) irq=true; }
    void clearDone() { status&=~DONE_B; if (state==DONE) state=IDLE; status|=IDLE_B; }
};

TEST(StateMachine, B_1_1_IdleToBusy) {
    MockNpu npu;
    ASSERT_EQ(npu.state, MockNpu::IDLE);
    npu.writeCtrl(MockNpu::START);
    EXPECT_EQ(npu.state, MockNpu::FETCH_INSTR);
    EXPECT_TRUE(npu.status & MockNpu::BUSY_B);
}

TEST(StateMachine, B_1_4_ResetFromAnyState) {
    for (auto s : {MockNpu::IDLE, MockNpu::FETCH_INSTR, MockNpu::EXECUTING, MockNpu::DMA_WAIT, MockNpu::DONE}) {
        MockNpu npu; npu.state=s; npu.status=0xFF; npu.ctrl=0xFF; npu.irq=true;
        npu.writeCtrl(MockNpu::RESET);
        EXPECT_EQ(npu.state, MockNpu::IDLE);
        EXPECT_EQ(npu.ctrl, 0);
        EXPECT_EQ(npu.status, MockNpu::IDLE_B);
        EXPECT_FALSE(npu.irq);
    }
}

TEST(StateMachine, B_1_5_DoubleStartIgnored) {
    MockNpu npu;
    npu.writeCtrl(MockNpu::START);
    ASSERT_EQ(npu.state, MockNpu::FETCH_INSTR);
    npu.writeCtrl(MockNpu::START);
    EXPECT_EQ(npu.state, MockNpu::FETCH_INSTR);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
