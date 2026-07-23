#include <gtest/gtest.h>
#include <vector>
#include <cstdint>

struct Chunker {
    uint32_t bw;
    struct Chunk { uint32_t off, sz; };
    std::vector<Chunk> calc(uint32_t size) const {
        std::vector<Chunk> c; uint32_t rem=size, off=0;
        while (rem>0) { uint32_t cs=std::min(bw, rem); c.push_back({off, cs}); off+=cs; rem-=cs; }
        return c;
    }
};

TEST(DMA, B_2_1_ChunkingExact) {
    Chunker ch{32};
    auto c=ch.calc(1024);
    EXPECT_EQ(c.size(), 32);
    for (size_t i=0;i<c.size();++i) { EXPECT_EQ(c[i].sz, 32); EXPECT_EQ(c[i].off, i*32); }
}

TEST(DMA, B_2_2_ChunkingNonAligned) {
    Chunker ch{32};
    auto c=ch.calc(1000);
    EXPECT_EQ(c.size(), 32);
    for (size_t i=0;i<31;++i) EXPECT_EQ(c[i].sz, 32);
    EXPECT_EQ(c[31].sz, 8);
}

TEST(DMA, B_2_7_ZeroSize) {
    Chunker ch{32};
    auto c=ch.calc(0);
    EXPECT_EQ(c.size(), 0);
}

TEST(DMA, B_2_12_BusWidth16) {
    Chunker ch{16};
    auto c=ch.calc(100);
    EXPECT_EQ(c.size(), 7);
    EXPECT_EQ(c[6].sz, 4);
}

TEST(DMA, B_2_13_BusWidth64) {
    Chunker ch{64};
    auto c=ch.calc(1000);
    EXPECT_EQ(c.size(), 16);
    EXPECT_EQ(c[15].sz, 40);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
