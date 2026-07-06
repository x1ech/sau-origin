#include "sau/trace_writer.hh"

#include <iomanip>
#include <stdexcept>

namespace gem5::sau
{
namespace
{

const char *
eventName(EventKind event)
{
    switch (event) {
      case EventKind::CommandAccepted:
        return "command_accepted";
      case EventKind::ReadAccepted:
        return "read_accepted";
      case EventKind::ReadResponseVisible:
        return "read_response_visible";
      case EventKind::ArrayInputAccepted:
        return "array_input_accepted";
      case EventKind::ResultProduced:
        return "result_produced";
      case EventKind::WriteAccepted:
        return "write_accepted";
      case EventKind::PhaseChanged:
        return "phase_changed";
      case EventKind::CommandComplete:
        return "command_complete";
    }

    throw std::invalid_argument("unknown SAU event kind");
}

const char *
phaseName(Phase phase)
{
    switch (phase) {
      case Phase::Idle:
        return "idle";
      case Phase::OperandLoad:
        return "operand_load";
      case Phase::ArrayActive:
        return "array_active";
      case Phase::ArrayDrain:
        return "array_drain";
      case Phase::Writeback:
        return "writeback";
      case Phase::Complete:
        return "complete";
    }

    throw std::invalid_argument("unknown SAU phase");
}

} // anonymous namespace

TraceWriter::TraceWriter(const std::string &path)
{
    if (path.empty()) {
        return;
    }

    output.open(path, std::ios::out | std::ios::trunc);
    if (!output.is_open()) {
        throw std::runtime_error("failed to open SAU trace file: " + path);
    }

    output << "cycle,event,command_id,stream,address,beat,phase\n";
}

bool
TraceWriter::enabled() const
{
    return output.is_open();
}

void
TraceWriter::emit(uint64_t cycle, EventKind event, uint64_t commandId,
                  std::string_view stream, Addr address, uint32_t beat,
                  Phase phase)
{
    if (!enabled()) {
        return;
    }

    output << cycle << ',' << eventName(event) << ',' << commandId << ','
           << stream << ",0x" << std::hex << std::setw(8)
           << std::setfill('0') << address << std::dec << std::setfill(' ')
           << ',' << beat << ',' << phaseName(phase) << '\n';

    if (event == EventKind::CommandComplete) {
        output.flush();
    }
}

} // namespace gem5::sau
