#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
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
        respondPacket(packet);
    }

    void
    respondBack()
    {
        PacketPtr packet = requests.back();
        requests.pop_back();
        respondPacket(packet);
    }

    void
    retry()
    {
        sendRetryReq();
    }

    static uint8_t
    readPatternByte(Addr address, unsigned byte)
    {
        return static_cast<uint8_t>(address + 3 * byte);
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
    recvFunctional(PacketPtr packet) override
    {
        const Addr base = packet->getAddr();
        if (packet->isWrite()) {
            const uint8_t *data = packet->getConstPtr<uint8_t>();
            for (unsigned byte = 0; byte < packet->getSize(); ++byte) {
                functionalStore[base + byte] = data[byte];
            }
        } else {
            uint8_t *data = packet->getPtr<uint8_t>();
            for (unsigned byte = 0; byte < packet->getSize(); ++byte) {
                const auto entry = functionalStore.find(base + byte);
                data[byte] =
                    entry == functionalStore.end() ? 0 : entry->second;
            }
        }
        packet->makeResponse();
    }

    void
    recvRespRetry() override
    {
        FAIL() << "SAU request port must accept timing responses";
    }

  private:
    void
    respondPacket(PacketPtr packet)
    {
        if (packet->isRead()) {
            // Serve a deterministic address-derived payload so the test
            // can check that the port surfaces the real response bytes.
            uint8_t *data = packet->getPtr<uint8_t>();
            for (unsigned byte = 0; byte < packet->getSize(); ++byte) {
                data[byte] = readPatternByte(packet->getAddr(), byte);
            }
        }
        packet->makeResponse();
        ASSERT_TRUE(sendTimingResp(packet));
    }

    bool accepting = true;
    std::deque<PacketPtr> requests;
    std::map<Addr, uint8_t> functionalStore;
};

const Beat readBeat{StreamKind::OperandA, 0x1000, 3, false};
const Beat writeBeat{StreamKind::Output, 0x3000, 7, true};

std::vector<uint8_t>
testWritePayload()
{
    std::vector<uint8_t> payload(32, 0);
    for (unsigned byte = 0; byte < payload.size(); ++byte) {
        payload[byte] = static_cast<uint8_t>(0x90 + byte);
    }
    return payload;
}

TEST(SauMemoryPort, SendsExactReadBeatAndSurfacesResponsePayload)
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

    auto responses = port.takeVisibleResponses();
    ASSERT_EQ(responses.size(), 1);
    EXPECT_EQ(responses.front().beat, readBeat);
    ASSERT_EQ(responses.front().data.size(), 32u);
    for (unsigned byte = 0; byte < 32; ++byte) {
        EXPECT_EQ(responses.front().data[byte],
                  TestMemory::readPatternByte(readBeat.address, byte));
    }
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

TEST(SauMemoryPort, CarriesRealWritePayload)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);

    const std::vector<uint8_t> payload = testWritePayload();
    ASSERT_TRUE(port.trySend(writeBeat, true, payload.data()));

    const PacketPtr packet = memory.front();
    ASSERT_TRUE(packet->isWrite());
    EXPECT_EQ(packet->getAddr(), writeBeat.address);
    const uint8_t *data = packet->getConstPtr<uint8_t>();
    EXPECT_TRUE(std::equal(payload.begin(), payload.end(), data));

    memory.respond();
    EXPECT_EQ(port.outstandingWrites(), 0);
    EXPECT_TRUE(port.takeVisibleResponses().empty());
}

TEST(SauMemoryPort, FunctionalAccessesRoundTripAcrossChunks)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);

    // 5000 bytes force the 4096-byte functional chunking to split the
    // transfer; the readback must still be byte-exact and must not touch
    // any timing-side owner callbacks or outstanding counters.
    std::vector<uint8_t> image(5000);
    for (unsigned byte = 0; byte < image.size(); ++byte) {
        image[byte] = static_cast<uint8_t>(7 * byte + 1);
    }
    port.writeFunctional(0x29120000, image.data(), image.size());

    std::vector<uint8_t> readback(image.size(), 0);
    port.readFunctional(0x29120000, readback.data(), readback.size());
    EXPECT_EQ(readback, image);
    EXPECT_TRUE(owner.accepted.empty());
    EXPECT_EQ(owner.responseNotifications, 0);
    EXPECT_EQ(port.outstandingReads(), 0);
    EXPECT_EQ(port.outstandingWrites(), 0);
}

TEST(SauMemoryPort, BlockedWritePayloadSurvivesRetry)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);
    memory.allowRequests(false);

    // The payload buffer is intentionally destroyed after the rejected
    // trySend: the blocked packet must own an unchanged copy of the
    // address, beat metadata, and payload bytes until retry delivery.
    {
        const std::vector<uint8_t> payload = testWritePayload();
        EXPECT_FALSE(port.trySend(writeBeat, true, payload.data()));
    }
    EXPECT_TRUE(port.hasBlockedPacket());
    EXPECT_TRUE(owner.accepted.empty());

    memory.allowRequests(true);
    memory.retry();

    ASSERT_EQ(owner.accepted.size(), 1);
    EXPECT_EQ(owner.accepted.front(),
              std::make_pair(writeBeat, true));
    EXPECT_FALSE(port.hasBlockedPacket());

    const PacketPtr packet = memory.front();
    ASSERT_TRUE(packet->isWrite());
    EXPECT_EQ(packet->getAddr(), writeBeat.address);
    const std::vector<uint8_t> expected = testWritePayload();
    const uint8_t *data = packet->getConstPtr<uint8_t>();
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), data));

    memory.respond();
    EXPECT_EQ(port.outstandingWrites(), 0);
}

TEST(SauMemoryPort, DelayedReadResponseDoesNotBlockIndependentTraffic)
{
    GTestTickHandler tickHandler;
    TestMemory memory;
    TestOwner owner;
    SauMemoryPort port("test_port", owner, 17, 32);
    port.bind(memory);

    // The first read's response stays pending: a delayed response must
    // not occupy the request path the way a rejected packet does.
    ASSERT_TRUE(port.trySend(readBeat, false));
    EXPECT_EQ(port.outstandingReads(), 1);
    EXPECT_TRUE(port.canIssue());
    EXPECT_FALSE(port.hasBlockedPacket());

    const std::vector<uint8_t> payload = testWritePayload();
    ASSERT_TRUE(port.trySend(writeBeat, true, payload.data()));
    EXPECT_EQ(port.outstandingReads(), 1);
    EXPECT_EQ(port.outstandingWrites(), 1);

    // The independent write completes while the read is still in flight;
    // only the read's own outstanding count remains held.
    memory.respondBack();
    EXPECT_EQ(port.outstandingWrites(), 0);
    EXPECT_EQ(port.outstandingReads(), 1);
    EXPECT_EQ(owner.responseNotifications, 1);
    EXPECT_TRUE(port.takeVisibleResponses().empty());

    // Further issue also continues behind the delayed response.
    const Beat secondReadBeat{StreamKind::OperandB, 0x2000, 4, false};
    ASSERT_TRUE(port.trySend(secondReadBeat, false));
    EXPECT_EQ(port.outstandingReads(), 2);
    ASSERT_EQ(owner.accepted.size(), 3);

    memory.respond();
    memory.respond();
    EXPECT_EQ(port.outstandingReads(), 0);
    EXPECT_EQ(owner.responseNotifications, 3);

    // The delayed response finally surfaces with its beat metadata and
    // payload intact, ahead of the later read.
    auto responses = port.takeVisibleResponses();
    ASSERT_EQ(responses.size(), 2);
    EXPECT_EQ(responses[0].beat, readBeat);
    EXPECT_EQ(responses[1].beat, secondReadBeat);
    ASSERT_EQ(responses[0].data.size(), 32u);
    ASSERT_EQ(responses[1].data.size(), 32u);
    for (unsigned byte = 0; byte < 32; ++byte) {
        EXPECT_EQ(responses[0].data[byte],
                  TestMemory::readPatternByte(readBeat.address, byte));
        EXPECT_EQ(responses[1].data[byte],
                  TestMemory::readPatternByte(secondReadBeat.address, byte));
    }
}

} // anonymous namespace
} // namespace gem5::sau
