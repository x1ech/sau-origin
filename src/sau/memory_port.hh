#ifndef __SAU_MEMORY_PORT_HH__
#define __SAU_MEMORY_PORT_HH__

#include <string>
#include <vector>

#include "mem/port.hh"
#include "sau/types.hh"

namespace gem5::sau
{

class SauMemoryPortOwner
{
  public:
    virtual ~SauMemoryPortOwner() = default;

    virtual void requestAccepted(const Beat &beat, bool write) = 0;
    virtual void responseAvailable() = 0;
};

class SauMemoryPort : public RequestPort
{
  public:
    SauMemoryPort(const std::string &name, SauMemoryPortOwner &owner,
                  RequestorID requestorId, unsigned beatBytes);
    ~SauMemoryPort() override;

    bool trySend(const Beat &beat, bool write);
    std::vector<Beat> takeVisibleResponses();

    bool canIssue() const;
    bool hasBlockedPacket() const;
    unsigned outstandingReads() const;
    unsigned outstandingWrites() const;

  protected:
    bool recvTimingResp(PacketPtr packet) override;
    void recvReqRetry() override;

  private:
    struct RequestState;

    SauMemoryPortOwner &owner;
    const RequestorID requestorId;
    const unsigned beatBytes;

    PacketPtr blockedPacket = nullptr;
    unsigned readCount = 0;
    unsigned writeCount = 0;
    std::vector<Beat> visibleResponses;

    void recordAcceptance(PacketPtr packet);
    void discardPacket(PacketPtr packet);
};

} // namespace gem5::sau

#endif // __SAU_MEMORY_PORT_HH__
