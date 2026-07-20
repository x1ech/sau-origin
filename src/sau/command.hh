#ifndef __SAU_COMMAND_HH__
#define __SAU_COMMAND_HH__

#include "sau/types.hh"

namespace gem5::sau
{

void validateCommand(const SauCommand &command, unsigned beatBytes);

uint32_t effectiveScheduleInstructions(const SauCommand &command);

} // namespace gem5::sau

#endif // __SAU_COMMAND_HH__
