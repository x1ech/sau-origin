#ifndef __SAU_TRACE_WRITER_HH__
#define __SAU_TRACE_WRITER_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

#include "sau/types.hh"

namespace gem5::sau
{

class TraceWriter
{
  public:
    explicit TraceWriter(const std::string &path);

    bool enabled() const;

    void emit(uint64_t cycle, EventKind event, uint64_t commandId,
              std::string_view stream, Addr address, uint32_t beat,
              Phase phase);

  private:
    std::ofstream output;
};

} // namespace gem5::sau

#endif // __SAU_TRACE_WRITER_HH__
