// standalone_test.cpp - verifies the Verilated npu_mac_core model behaves
// correctly (drives it exactly the way our future gem5 SimObject wrapper
// will: posedge clock, start pulse, streaming in_valid/in_ready handshake,
// wait for done) BEFORE we invest time wiring it into gem5. This is the
// same "verify standalone before integrating" discipline used earlier for
// the RISC-V benchmark binaries in this project.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "Vnpu_mac_core.h"
#include "verilated.h"

static vluint64_t sim_time = 0;

static void
tick(Vnpu_mac_core *dut)
{
    // One full clock period = two edges (matches how a gem5 SimObject
    // would drive this: toggle clk, eval(), toggle clk, eval()).
    dut->clk = 0;
    dut->eval();
    sim_time++;
    dut->clk = 1;
    dut->eval();
    sim_time++;
}

int
main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    Vnpu_mac_core *dut = new Vnpu_mac_core;

    // --- reset ---
    dut->rst_n = 0;
    dut->start = 0;
    dut->in_valid = 0;
    dut->in_a = 0;
    dut->in_b = 0;
    for (int i = 0; i < 4; i++) tick(dut);
    dut->rst_n = 1;
    tick(dut);

    // --- test vector: known inputs, compute expected sum in C++ ---
    std::vector<int16_t> a = {1, 2, 3, 4, 5, 6, 7, 8};
    std::vector<int16_t> b = {10, 10, 10, 10, 2, 2, 2, 2};
    int64_t expected = 0;
    for (size_t i = 0; i < a.size(); i++)
        expected += (int64_t)a[i] * (int64_t)b[i];

    // --- start the op ---
    dut->start = 1;
    dut->length = (uint16_t)a.size();
    tick(dut);
    dut->start = 0;

    uint64_t cyclesElapsed = 0;
    size_t idx = 0;
    bool done = false;
    int32_t result = 0;

    // Drive the streaming handshake until the RTL asserts done, or we hit
    // a generous safety bound (guards against an infinite loop if the RTL
    // or the driver logic has a bug).
    for (int guard = 0; guard < 1000 && !done; guard++) {
        if (idx < a.size()) {
            dut->in_valid = 1;
            dut->in_a = (uint16_t)a[idx];
            dut->in_b = (uint16_t)b[idx];
        } else {
            dut->in_valid = 0;
        }

        tick(dut);
        cyclesElapsed++;

        if (dut->in_valid && dut->in_ready)
            idx++;

        if (dut->done) {
            done = true;
            result = (int32_t)dut->result;
        }
    }

    printf("expected = %lld\n", (long long)expected);
    printf("rtl_result = %d\n", result);
    printf("cycles_elapsed (this op) = %llu\n", (unsigned long long)cyclesElapsed);
    printf("match = %s\n", (result == expected) ? "YES" : "NO");

    dut->final();
    delete dut;
    return (result == expected) ? 0 : 1;
}
