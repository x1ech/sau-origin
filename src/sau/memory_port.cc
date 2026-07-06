#include "sau/memory_port.hh"

#include <cstring>
#include <memory>

#include "base/logging.hh"
#include "mem/packet.hh"
#include "mem/request.hh"

namespace gem5::sau
{

struct SauMemoryPort::RequestState : public Packet::SenderState
{
    RequestState(const Beat &beat, bool write) : beat(beat), write(write) {}

    const Beat beat;
    const bool write;
};

SauMemoryPort::SauMemoryPort(const std::string &name,
                             SauMemoryPortOwner &owner,
                             RequestorID requestorId, unsigned beatBytes)
    : RequestPort(name), owner(owner), requestorId(requestorId),
      beatBytes(beatBytes)
{
    panic_if(beatBytes == 0, "SAU memory beat size must be nonzero");
}

SauMemoryPort::~SauMemoryPort()
{
    if (blockedPacket) {
        discardPacket(blockedPacket);
    }
}

bool
SauMemoryPort::trySend(const Beat &beat, bool write)
{
    panic_if(blockedPacket, "SAU cannot issue while a packet is blocked");

    const auto request = std::make_shared<Request>(
        beat.address, beatBytes, 0, requestorId);
    auto *packet = new Packet(
        request, write ? MemCmd::WriteReq : MemCmd::ReadReq);
    packet->allocate();
    packet->pushSenderState(new RequestState(beat, write));

    if (write) {
        std::memset(packet->getPtr<uint8_t>(), 0, beatBytes);
    }

    if (!sendTimingReq(packet)) {
        blockedPacket = packet;
        return false;
    }

    recordAcceptance(packet);
    return true;
}

std::vector<Beat>
SauMemoryPort::takeVisibleResponses()
{
    std::vector<Beat> responses;
    responses.swap(visibleResponses);
    return responses;
}

bool
SauMemoryPort::canIssue() const
{
    return blockedPacket == nullptr;
}

bool
SauMemoryPort::hasBlockedPacket() const
{
    return blockedPacket != nullptr;
}

unsigned
SauMemoryPort::outstandingReads() const
{
    return readCount;
}

unsigned
SauMemoryPort::outstandingWrites() const
{
    return writeCount;
}

bool
SauMemoryPort::recvTimingResp(PacketPtr packet)
{
    panic_if(!packet->isResponse(),
             "SAU memory port received a non-response packet");

    auto *state = dynamic_cast<RequestState *>(packet->popSenderState());
    panic_if(!state, "SAU memory response has no request state");

    if (state->write) {
        panic_if(writeCount == 0,
                 "SAU write response has no outstanding request");
        --writeCount;
    } else {
        panic_if(readCount == 0,
                 "SAU read response has no outstanding request");
        --readCount;
        visibleResponses.push_back(state->beat);
    }

    delete state;
    delete packet;
    owner.responseAvailable();
    return true;
}

void
SauMemoryPort::recvReqRetry()
{
    panic_if(!blockedPacket,
             "SAU received a request retry without a blocked packet");

    PacketPtr packet = blockedPacket;
    if (!sendTimingReq(packet)) {
        return;
    }

    blockedPacket = nullptr;
    recordAcceptance(packet);
}

void
SauMemoryPort::recordAcceptance(PacketPtr packet)
{
    auto *state = dynamic_cast<RequestState *>(packet->senderState);
    panic_if(!state, "SAU accepted packet has no request state");

    if (state->write) {
        ++writeCount;
    } else {
        ++readCount;
    }
    owner.requestAccepted(state->beat, state->write);
}

void
SauMemoryPort::discardPacket(PacketPtr packet)
{
    auto *state = dynamic_cast<RequestState *>(packet->popSenderState());
    panic_if(!state, "SAU blocked packet has no request state");
    delete state;
    delete packet;
}

} // namespace gem5::sau
