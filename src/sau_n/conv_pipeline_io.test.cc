#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sau_n/conv_pipeline_io.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
ioConfig()
{
    PipelineResolvedConfig config;
    config.name = "io";
    config.im2col.name = "io_im2col";
    config.im2col.n = 1;
    config.im2col.c = 1;
    config.im2col.h = 1;
    config.im2col.w = 1;
    config.im2col.outH = 1;
    config.im2col.outW = 1;
    config.im2col.kernelH = 3;
    config.im2col.kernelW = 3;
    config.im2col.strideH = 1;
    config.im2col.strideW = 1;
    config.im2col.dilationH = 1;
    config.im2col.dilationW = 1;
    config.im2col.padTop = 1;
    config.im2col.padLeft = 1;
    config.outChannels = 2;
    return config;
}

TEST(ConvPipelineIo, WritesNchwSignedDecimalOutput)
{
    const std::string path = "/tmp/sau_n_conv_pipeline_output.csv";
    writeConvPipelineOutput(path, ioConfig(), {-128, 127});

    std::ifstream input(path);
    std::string text(
        (std::istreambuf_iterator<char>(input)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(text, "n,oc,oh,ow,value\n0,0,0,0,-128\n0,1,0,0,127\n");
    std::remove(path.c_str());
}

TEST(ConvPipelineIo, WritesNormalizedCanonicalCycleTrace)
{
    const std::string path = "/tmp/sau_n_conv_pipeline_trace.csv";
    const std::string hash(64, 'a');
    {
        ConvPipelineTraceWriter writer(path, hash);
        ConvPipelineCycle cycle;
        cycle.cycle = 7;
        cycle.state = PipelineState::StreamK;
        cycle.tileIndex = 2;
        cycle.im2col.feedValid = true;
        cycle.im2col.feedReady = true;
        cycle.im2col.feed.data[0] = 0x81;
        cycle.im2col.feed.mask = 1;
        cycle.sau.state = SauEngineState::Start;
        cycle.sau.rowScoreValid = true;
        cycle.sau.outputSlots[0] = 0xff80;
        cycle.sau.peValidMask[0] = 1;
        cycle.sau.macCommitMask[0] = 1;
        cycle.sau.peStates[0].activation = static_cast<int8_t>(0x81);
        cycle.sau.peStates[0].weight = 0x7f;
        cycle.sau.peStates[0].accumulator = -1;
        cycle.sau.peStates[1].activation = 0x55;
        cycle.sau.peStates[1].weight = 0x66;
        cycle.sau.peStates[1].accumulator = 0x123456;
        writer.emit(cycle);
    }
    std::ifstream input(path);
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(input, header));
    ASSERT_TRUE(std::getline(input, row));
    EXPECT_EQ(
        static_cast<std::size_t>(
            std::count(header.begin(), header.end(), ',')),
        CanonicalPipelineTraceFields.size() - 1);
    EXPECT_NE(header.find("resolved_config_sha256"), std::string::npos);
    EXPECT_NE(row.find(hash), std::string::npos);
    EXPECT_NE(
        row.find("0x00000000000000000000000000000081"),
        std::string::npos);
    EXPECT_NE(
        row.find(
            "0x00000000000000000000000000000000"
            "0000000000000000000000000000ff80"),
        std::string::npos);
    EXPECT_NE(
        row.find("0x" + std::string(510, '0') + "81"),
        std::string::npos);
    EXPECT_NE(
        row.find("0x" + std::string(510, '0') + "7f"),
        std::string::npos);
    EXPECT_NE(
        row.find("0x" + std::string(1530, '0') + "ffffff"),
        std::string::npos);
    std::remove(path.c_str());
}

TEST(ConvPipelineIo, RejectsBadHashAndOutputCount)
{
    EXPECT_THROW(ConvPipelineTraceWriter("", "bad"), std::invalid_argument);
    EXPECT_THROW(
        writeConvPipelineOutput("/tmp/unused.csv", ioConfig(), {1}),
        std::invalid_argument);
}

TEST(ConvPipelineIo, WritesEveryCycleThroughPipelineDrained)
{
    const std::string path = "/tmp/sau_n_conv_pipeline_full_trace.csv";
    ConvPipelineModel model(ioConfig());
    uint64_t emitted = 0;
    uint64_t lines = 0;
    std::string last;
    {
        ConvPipelineTraceWriter writer(path, std::string(64, 'b'));
        while (!model.hasDrained()) {
            const auto cycle = model.tick();
            writer.emit(cycle);
            ++emitted;
        }

        std::ifstream input(path);
        std::string line;
        while (std::getline(input, line)) {
            EXPECT_EQ(
                static_cast<std::size_t>(
                    std::count(line.begin(), line.end(), ',')),
                CanonicalPipelineTraceFields.size() - 1);
            last = line;
            ++lines;
        }
    }
    EXPECT_EQ(lines, emitted + 1);
    ASSERT_GE(last.size(), std::size_t{2});
    EXPECT_EQ(last.substr(last.size() - 2), ",1");
    std::remove(path.c_str());
}

} // anonymous namespace
} // namespace gem5::sau_n
