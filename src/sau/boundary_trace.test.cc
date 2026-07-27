#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "sau/boundary_trace.hh"

namespace gem5::sau
{
namespace
{

constexpr char PackageDirectory[] =
    "tests/gem5/sau/functional_ref/int8_gemm_32x32x32_abtd_boundary";

std::string
temporaryPath(const char *name)
{
    return std::string(testing::TempDir()) + name;
}

TEST(BoundaryTraceWriter, WritesPerSignalCyclesAndHexValues)
{
    const std::string path = temporaryPath("sau_boundary_writer.csv");
    {
        BoundaryTraceWriter writer(path);
        MemoryBeat256 beat;
        beat.bytes[0] = 0x1e;
        beat.bytes[31] = 0xf1;
        writer.emitZero("a");
        writer.emit("a", beat);
        writer.emitZero("b");
        writer.emit("b", 42, beat);
        writer.emitZero("b");
        ASSERT_TRUE(writer.good());
    }

    std::ifstream input(path);
    std::stringstream content;
    content << input.rdbuf();
    EXPECT_EQ(content.str(),
              "signal,cycle,value\n"
              "a,0,0x0\n"
              "a,1,0xf1000000000000000000000000000000"
              "0000000000000000000000000000001e\n"
              "b,0,0x0\n"
              "b,42,0xf1000000000000000000000000000000"
              "0000000000000000000000000000001e\n"
              "b,43,0x0\n");
    std::remove(path.c_str());
}

TEST(BoundaryTraceWriter, FlushesBeforeDestruction)
{
    const std::string path = temporaryPath("sau_boundary_flush.csv");
    {
        BoundaryTraceWriter writer(path);
        writer.emitZero("a");
        writer.flush();
        ASSERT_TRUE(writer.good());

        std::ifstream input(path);
        std::stringstream content;
        content << input.rdbuf();
        EXPECT_EQ(content.str(),
                  "signal,cycle,value\n"
                  "a,0,0x0\n");
    }
    std::remove(path.c_str());
}

TEST(BoundaryTrace, GeneratesTheAbtdModelTrace)
{
    // Runs from the gem5 repository root; skip when the functional_ref
    // package is not reachable from the current directory.
    {
        std::ifstream probe(std::string(PackageDirectory) +
                            "/initial_memory.hex");
        if (!probe.is_open()) {
            GTEST_SKIP() << "ABTD package not visible from this directory";
        }
    }

    const std::string path = temporaryPath("sau_abtd_model_boundary.csv");
    ASSERT_TRUE(generateAbtdBoundaryTrace(PackageDirectory, path));

    // 1 header + rdata (1 + 64) + data_A (64: the reuse-A double
    // readout) + data_B (33: the leaked resident tail plus the 32
    // streamed beats) + trans0_inRow (32) + outCol (1 + 32).
    std::ifstream input(path);
    std::string line;
    unsigned lines = 0;
    unsigned rdata = 0;
    unsigned dataB = 0;
    while (std::getline(input, line)) {
        ++lines;
        if (line.rfind("sau_sram_rdata,", 0) == 0) {
            ++rdata;
        }
        if (line.rfind("data_B,", 0) == 0) {
            ++dataB;
        }
    }
    EXPECT_EQ(lines, 1u + 65u + 64u + 33u + 32u + 33u);
    EXPECT_EQ(rdata, 65u);
    EXPECT_EQ(dataB, 33u);
    std::remove(path.c_str());
}

} // anonymous namespace
} // namespace gem5::sau
