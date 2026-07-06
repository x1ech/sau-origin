#include <gtest/gtest.h>

#include <algorithm>
#include <deque>
#include <utility>
#include <vector>

#include "base/gtest/cur_tick_fake.hh"
#include "mem/packet.hh"
#include "mem/port.hh"
#include "sau/memory_port.hh"

namespace gem5::sau
{
namespace
{

class TestOwner : public SauMemoryPortOwner
{
  public:
    void
    requestAccepted(const Beat &beat, bool write) override
    {
        accepted.emplace_back(beat, write);
    }

    void
    responseAvailable() override
    {
        ++responseNotifications;
    }

    std::vector<std::pair<Beat, bool>> accepted;
    unsigned responseNotifications = 0;
};

class TestMemory : public ResponsePort
{
  public:
    TestMemory() : ResponsePort("test_memory") {}

    AddrRangeList
    getAddrRanges() const override
    {
        return {};
    }

    void
    allowRequests(bool allow)
    {
        accepting = allow;
    }

    PacketPtr
    front() const
    {
        return requests.front();
    }

    void
    respond()
    {
        PacketPtr packet = requests.front();
        requests.pop_front();
        packet->makeResponse();
        ASSERT_TRUE(sendTimingResp(packet));
    }

    void
    retry()
    {
        sendRetryReq();
    }

  protected:
    bool
    recvTimingReq(PacketPtr packet) override
    {
        if (!accepting) {
            return false;
        }
        requests.push_back(packet);
        return true;
    }

    Tick
    recvAtomic(PacketPtr) override
    {
        return 0;
    }

    void
    recvFunctional(PacketPtr) override
    {
    }

    void
    recvRespRetry() override
    {
        FAIL() << "SAU request port must accept timing responses";
    }

  private:
    bool accepting = true;
    std::deque<PacketPtr> requests;
};

const Beat readBeat{StreamKind::OperandA, 0x1000, 3, false};
const Beat writeBeat{StreamKind::Output, 0x3000, 7, true};

TEST(SauMemoryPort, SendsExactReadBeatAndTracksResponse)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);

    ASSERT_TRUE(port.trySend(readBeat, false));
    ASSERT_EQ(owner.accepted.size(), 1);
    EXPECT_EQ(owner.accepted.front(),
              std::make_pair(readBeat, false));
    EXPECT_EQ(port.outstandingReads(), 1);
    EXPECT_EQ(port.outstandingWrites(), 0);

    const PacketPtr packet = memory.front();
    EXPECT_TRUE(packet->isRead());
    EXPECT_EQ(packet->getAddr(), readBeat.address);
    EXPECT_EQ(packet->getSize(), 32);
    EXPECT_EQ(packet->requestorId(), 17);

    memory.respond();
    EXPECT_EQ(port.outstandingReads(), 0);
    EXPECT_EQ(owner.responseNotifications, 1);
    EXPECT_EQ(port.takeVisibleResponses(),
              std::vector<Beat>{readBeat});
    EXPECT_TRUE(port.takeVisibleResponses().empty());
}

TEST(SauMemoryPort, RetainsRejectedPacketUntilRetryAcceptance)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);
    memory.allowRequests(false);

    EXPECT_FALSE(port.trySend(readBeat, false));
    EXPECT_TRUE(port.hasBlockedPacket());
    EXPECT_FALSE(port.canIssue());
    EXPECT_TRUE(owner.accepted.empty());
    EXPECT_EQ(port.outstandingReads(), 0);

    memory.retry();
    EXPECT_TRUE(port.hasBlockedPacket());
    EXPECT_TRUE(owner.accepted.empty());
    EXPECT_EQ(port.outstandingReads(), 0);

    memory.allowRequests(true);
    memory.retry();

    ASSERT_EQ(owner.accepted.size(), 1);
    EXPECT_EQ(owner.accepted.front(),
              std::make_pair(readBeat, false));
    EXPECT_FALSE(port.hasBlockedPacket());
    EXPECT_TRUE(port.canIssue());
    EXPECT_EQ(port.outstandingReads(), 1);

    memory.respond();
}

TEST(SauMemoryPort, ZeroFillsWritesWithoutExposingWriteResponses)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);

    ASSERT_TRUE(port.trySend(writeBeat, true));
    EXPECT_EQ(port.outstandingReads(), 0);
    EXPECT_EQ(port.outstandingWrites(), 1);

    const PacketPtr packet = memory.front();
    ASSERT_TRUE(packet->isWrite());
    const uint8_t *data = packet->getConstPtr<uint8_t>();
    EXPECT_TRUE(std::all_of(data, data + packet->getSize(),
                            [](uint8_t byte) { return byte == 0; }));

    memory.respond();
    EXPECT_EQ(port.outstandingWrites(), 0);
    EXPECT_EQ(owner.responseNotifications, 1);
    EXPECT_TRUE(port.takeVisibleResponses().empty());
}

} // anonymous namespace
} // namespace gem5::sau
