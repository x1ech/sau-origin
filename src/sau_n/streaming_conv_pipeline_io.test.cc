#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "sau_n/streaming_conv_pipeline_io.hh"

namespace gem5::sau_n
{
namespace
{

PipelineResolvedConfig
traceConfig()
{
    PipelineResolvedConfig config;
    config.name = "streaming_trace";
    config.im2col.name = "streaming_trace_im2col";
    config.im2col.n = 1;
    config.im2col.c = 1;
    config.im2col.h = config.im2col.w = 3;
    config.im2col.outH = config.im2col.outW = 1;
    config.im2col.kernelH = config.im2col.kernelW = 3;
    config.im2col.strideH = config.im2col.strideW = 1;
    config.im2col.dilationH = config.im2col.dilationW = 1;
    config.im2col.padTop = config.im2col.padLeft = 0;
    config.outChannels = 1;
    config.cutbit = 0;
    config.weightGenerator = "ones";
    config.biasGenerator = "zero";
    return config;
}

std::size_t
commas(const std::string &line)
{
    return static_cast<std::size_t>(
        std::count(line.begin(), line.end(), ','));
}

std::vector<std::string>
fields(const std::string &line)
{
    std::vector<std::string> result;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        result.push_back(field);
    }
    return result;
}

TEST(StreamingConvPipelineIo, CompactTraceHasControlAndSharedSpadFields)
{
    const std::string path = "/tmp/sau_n_streaming_compact_trace.csv";
    {
        StreamingConvPipelineTraceWriter writer(
            path, std::string(64, 'a'), false);
        StreamingConvPipelineCycle cycle;
        cycle.cycle = 3;
        cycle.producer.s2Valid = true;
        cycle.producer.s2.compacted.spatialMask = 1;
        cycle.producer.s2.compacted.coordinates[0].valid = true;
        cycle.producer.s2.compacted.sourceLanes[0] = 7;
        cycle.bRequest.valid[2] = true;
        cycle.bRequest.address[2] = 9;
        cycle.bRequestK = 5;
        cycle.dQueueOccupancy = 1;
        cycle.dHeadPendingMask = 3;
        cycle.consumerState = StreamingConsumerState::AcceptK;
        writer.emit(cycle);
    }
    std::ifstream input(path);
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(input, header));
    ASSERT_TRUE(std::getline(input, row));
    EXPECT_EQ(commas(header), StreamingPipelineTraceFields.size() - 1);
    EXPECT_EQ(commas(row), StreamingPipelineTraceFields.size() - 1);
    EXPECT_EQ(header.find("pe_valid_mask"), std::string::npos);
    EXPECT_NE(header.find("s2_source_lanes"), std::string::npos);
    EXPECT_NE(header.find("b_request_mask"), std::string::npos);
    EXPECT_NE(header.find("d_head_pending_mask"), std::string::npos);
    EXPECT_NE(row.find("0000000000000007"), std::string::npos);
    const auto rowFields = fields(row);
    ASSERT_EQ(rowFields.size(), StreamingPipelineTraceFields.size());
    EXPECT_EQ(rowFields[0], "2");
    std::remove(path.c_str());
}

TEST(StreamingConvPipelineIo, DetailedTraceAddsCanonicalPeSnapshots)
{
    const std::string path = "/tmp/sau_n_streaming_detailed_trace.csv";
    {
        StreamingConvPipelineTraceWriter writer(
            path, std::string(64, 'b'), true);
        StreamingConvPipelineCycle cycle;
        cycle.sau.peValidMask[0] = 1;
        cycle.sau.macCommitMask[0] = 1;
        cycle.sau.peStates[0].activation = static_cast<int8_t>(0x81);
        cycle.sau.peStates[0].weight = 0x7f;
        cycle.sau.peStates[0].accumulator = -1;
        writer.emit(cycle);
    }
    std::ifstream input(path);
    std::string header;
    std::string row;
    ASSERT_TRUE(std::getline(input, header));
    ASSERT_TRUE(std::getline(input, row));
    const std::size_t fields = StreamingPipelineTraceFields.size() +
        StreamingPipelineDetailedTraceFields.size();
    EXPECT_EQ(commas(header), fields - 1);
    EXPECT_EQ(commas(row), fields - 1);
    EXPECT_NE(header.find("pe_valid_mask"), std::string::npos);
    EXPECT_NE(row.find("0x" + std::string(510, '0') + "81"),
              std::string::npos);
    EXPECT_NE(row.find("0x" + std::string(1530, '0') + "ffffff"),
              std::string::npos);
    std::remove(path.c_str());
}

TEST(StreamingConvPipelineIo, WritesEveryCycleThroughDrained)
{
    const std::string path = "/tmp/sau_n_streaming_full_trace.csv";
    StreamingConvPipelineModel model(traceConfig());
    uint64_t emitted = 0;
    {
        StreamingConvPipelineTraceWriter writer(
            path, std::string(64, 'c'), false);
        while (!model.hasDrained()) {
            writer.emit(model.tick());
            ++emitted;
        }
    }
    std::ifstream input(path);
    std::string line;
    uint64_t lines = 0;
    std::string last;
    while (std::getline(input, line)) {
        EXPECT_EQ(commas(line), StreamingPipelineTraceFields.size() - 1);
        last = line;
        ++lines;
    }
    EXPECT_EQ(lines, emitted + 1);
    const auto lastFields = fields(last);
    ASSERT_EQ(lastFields.size(), StreamingPipelineTraceFields.size());
    const auto drained = std::find(
        StreamingPipelineTraceFields.begin(),
        StreamingPipelineTraceFields.end(), "drained");
    ASSERT_NE(drained, StreamingPipelineTraceFields.end());
    EXPECT_EQ(
        lastFields[static_cast<std::size_t>(
            drained - StreamingPipelineTraceFields.begin())],
        "1");
    std::remove(path.c_str());
}

TEST(StreamingConvPipelineIo, RejectsInvalidResolvedHash)
{
    EXPECT_THROW(
        StreamingConvPipelineTraceWriter("", "bad", false),
        std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau_n
