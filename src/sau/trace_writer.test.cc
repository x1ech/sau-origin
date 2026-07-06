#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "sau/trace_writer.hh"

namespace gem5::sau
{
namespace
{

class TemporaryFile
{
  public:
    TemporaryFile()
    {
        char name[] = "/tmp/sau_trace_writer_XXXXXX";
        const int fd = mkstemp(name);
        EXPECT_NE(fd, -1);
        if (fd != -1) {
            close(fd);
            path = name;
        }
    }

    ~TemporaryFile()
    {
        if (!path.empty()) {
            std::remove(path.c_str());
        }
    }

    std::string path;
};

TEST(TraceWriter, WritesStableSchemaAndEventNames)
{
    TemporaryFile output;
    ASSERT_FALSE(output.path.empty());

    TraceWriter writer(output.path);
    writer.emit(0, EventKind::CommandAccepted, 1, "none", 0, 0,
                Phase::OperandLoad);
    writer.emit(7, EventKind::CommandComplete, 1, "none", 0, 0,
                Phase::Complete);

    std::ifstream trace(output.path);
    const std::string contents{
        std::istreambuf_iterator<char>(trace),
        std::istreambuf_iterator<char>()};

    EXPECT_EQ(contents,
              "cycle,event,command_id,stream,address,beat,phase\n"
              "0,command_accepted,1,none,0x00000000,0,operand_load\n"
              "7,command_complete,1,none,0x00000000,0,complete\n");
}

TEST(TraceWriter, EmptyPathDisablesOutput)
{
    TraceWriter writer("");

    EXPECT_FALSE(writer.enabled());
    EXPECT_NO_THROW(
        writer.emit(0, EventKind::CommandAccepted, 1, "none", 0, 0,
                    Phase::OperandLoad));
}

} // anonymous namespace
} // namespace gem5::sau
