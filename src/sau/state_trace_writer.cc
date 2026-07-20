#include "sau/state_trace_writer.hh"

#include <stdexcept>

namespace gem5::sau
{
namespace
{

const char *
stateName(SauScheduleState state)
{
    switch (state) {
      case SauScheduleState::Idle:
        return "idle";
      case SauScheduleState::ResidentLoad:
        return "resident_load";
      case SauScheduleState::TransposeSetup:
        return "transpose_setup";
      case SauScheduleState::FlowExecute:
        return "flow_execute";
      case SauScheduleState::FlowBoundary:
        return "flow_boundary";
      case SauScheduleState::DrainAndWriteback:
        return "drain_and_writeback";
      case SauScheduleState::Complete:
        return "complete";
    }
    throw std::invalid_argument("unknown SAU schedule state");
}

} // anonymous namespace

StateTraceWriter::StateTraceWriter(const std::string &path)
{
    if (path.empty()) {
        return;
    }
    output.open(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open SAU state trace: " + path);
    }
    output << "cycle,command_id,schedule_state,rtl_state,input_switch,"
           << "transition_cause\n";
}

bool
StateTraceWriter::enabled() const
{
    return output.is_open();
}

void
StateTraceWriter::emit(uint64_t cycle, uint64_t commandId,
                       SauScheduleState state, std::string_view inputSwitch,
                       std::string_view cause)
{
    if (!enabled()) {
        return;
    }
    output << cycle << ',' << commandId << ',' << stateName(state) << ','
           << scheduleStateMapping(state).rtlStates << ',' << inputSwitch
           << ',' << cause << '\n';
    output.flush();
}

} // namespace gem5::sau
