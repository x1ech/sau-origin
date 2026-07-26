#ifndef __SAU_MEMORY_PORT_HH__
#define __SAU_MEMORY_PORT_HH__

#include <cstdint>
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

/// One visible read response: the request beat metadata plus the real
/// beatBytes payload returned by the timing memory system.
struct SauMemoryResponse
{
    Beat beat;
    std::vector<uint8_t> data;
};

class SauMemoryPort : public RequestPort
{
  public:
    SauMemoryPort(const std::string &name, SauMemoryPortOwner &owner,
                  RequestorID requestorId, unsigned beatBytes);
    ~SauMemoryPort() override;

    /**
     * Issue one read or write beat.  A write carries the beatBytes
     * payload bytes; a null writePayload keeps the legacy timing-only
     * zero-filled contract.  Reads must not pass a payload.
     *
     * A rejected packet is retained unchanged - address, size, payload,
     * and beat metadata - until recvReqRetry() delivers it.
     */
    bool trySend(const Beat &beat, bool write,
                 const uint8_t *writePayload = nullptr);
    std::vector<SauMemoryResponse> takeVisibleResponses();

    /**
     * Debug-mode accesses for the timing-memory data authority: image
     * preload before the first command and the final-memory readback.
     * They bypass all timing state and must not be used while timing
     * requests are outstanding.
     */
    void writeFunctional(Addr address, const uint8_t *data, uint64_t size);
    void readFunctional(Addr address, uint8_t *data, uint64_t size);

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
    std::vector<SauMemoryResponse> visibleResponses;

    void recordAcceptance(PacketPtr packet);
    void discardPacket(PacketPtr packet);
};

} // namespace gem5::sau

#endif // __SAU_MEMORY_PORT_HH__
