#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "sau_n/im2col_trace.hh"

namespace gem5::sau_n
{
namespace
{

std::vector<std::string>
split(const std::string &line)
{
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = line.find(',', begin);
        fields.push_back(line.substr(begin, comma - begin));
        if (comma == std::string::npos) {
            return fields;
        }
        begin = comma + 1;
    }
}

TEST(Im2ColTraceWriterTest, WritesCanonicalHeaderAndNormalizedCycle)
{
    const std::string path = "/tmp/sau_n_im2col_trace_test.csv";
    const std::string hash(64, 'a');
    {
        Im2ColTraceWriter writer(path, hash);
        Im2ColCycle cycle;
        cycle.cycle = 3;
        cycle.state = Im2ColState::Collect;
        cycle.busy = true;
        cycle.fifoCount = 2;
        cycle.fifoReadPointer = 1;
        cycle.fifoWritePointer = 3;
        cycle.request.valid[0] = true;
        cycle.request.address[0] = 0xabc;
        cycle.request.address[1] = 0x123;
        cycle.request.valid[2] = true;
        cycle.request.address[2] = 5;
        cycle.response.valid[0] = true;
        cycle.response.data[0] = 0x12;
        cycle.response.data[1] = 0xff;
        cycle.response.valid[2] = true;
        cycle.response.data[2] = 0x34;
        cycle.feedValid = true;
        cycle.feedReady = false;
        cycle.feed.data[0] = 0x01;
        cycle.feed.data[15] = 0xab;
        cycle.feed.mask = 0x8001;
        cycle.drained = true;
        writer.emit(cycle);
    }

    std::ifstream input(path);
    std::string header;
    std::string row;
    std::string extra;
    ASSERT_TRUE(std::getline(input, header));
    ASSERT_TRUE(std::getline(input, row));
    EXPECT_FALSE(std::getline(input, extra));
    const auto headerFields = split(header);
    const auto fields = split(row);
    ASSERT_EQ(headerFields.size(), TraceFields.size());
    ASSERT_EQ(fields.size(), TraceFields.size());
    for (std::size_t index = 0; index < TraceFields.size(); ++index) {
        EXPECT_EQ(headerFields[index], TraceFields[index]);
    }

    EXPECT_EQ(fields[0], "1");
    EXPECT_EQ(fields[1], hash);
    EXPECT_EQ(fields[2], "3");
    EXPECT_EQ(fields[3], "2");
    EXPECT_EQ(fields[4], "1");
    EXPECT_EQ(fields[5], "0");
    EXPECT_EQ(fields[9], "0x0005");
    EXPECT_EQ(fields[10], "0xabc");
    EXPECT_EQ(fields[11], "0x000");
    EXPECT_EQ(fields[12], "0x005");
    EXPECT_EQ(fields[26], "0x0005");
    EXPECT_EQ(fields[27], "0x12");
    EXPECT_EQ(fields[28], "0x00");
    EXPECT_EQ(fields[29], "0x34");
    EXPECT_EQ(fields[43], "1");
    EXPECT_EQ(fields[44], "0");
    EXPECT_EQ(fields[45], "0xab000000000000000000000000000001");
    EXPECT_EQ(fields[46], "0x8001");
    std::remove(path.c_str());
}

TEST(Im2ColTraceWriterTest, NormalizesInvalidFeedAndAllowsDisabledOutput)
{
    const std::string hash(64, '0');
    EXPECT_NO_THROW(Im2ColTraceWriter disabled("", hash));

    const std::string path = "/tmp/sau_n_im2col_trace_invalid_feed.csv";
    {
        Im2ColTraceWriter writer(path, hash);
        Im2ColCycle cycle;
        cycle.feed.data.fill(0xff);
        cycle.feed.mask = 0xffff;
        writer.emit(cycle);
    }
    std::ifstream input(path);
    std::string line;
    ASSERT_TRUE(std::getline(input, line));
    ASSERT_TRUE(std::getline(input, line));
    const auto fields = split(line);
    EXPECT_EQ(fields[45], "0x00000000000000000000000000000000");
    EXPECT_EQ(fields[46], "0x0000");
    std::remove(path.c_str());
}

TEST(Im2ColTraceWriterTest, RejectsNonCanonicalSha256)
{
    EXPECT_THROW(Im2ColTraceWriter("", std::string(63, 'a')),
                 std::invalid_argument);
    EXPECT_THROW(Im2ColTraceWriter("", std::string(64, 'A')),
                 std::invalid_argument);
}

} // anonymous namespace
} // namespace gem5::sau_n
