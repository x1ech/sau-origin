#ifndef __SAU_STATE_TRACE_WRITER_HH__
#define __SAU_STATE_TRACE_WRITER_HH__

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>

#include "sau/schedule_state.hh"

namespace gem5::sau
{

/** Independent debug trace; architecture.csv's public schema stays unchanged. */
class StateTraceWriter
{
  public:
    explicit StateTraceWriter(const std::string &path);

    bool enabled() const;
    void emit(uint64_t cycle, uint64_t commandId, SauScheduleState state,
              std::string_view inputSwitch, std::string_view cause);

  private:
    std::ofstream output;
};

} // namespace gem5::sau

#endif // __SAU_STATE_TRACE_WRITER_HH__
