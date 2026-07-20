#ifndef __SAU_CSR_FIXTURE_HH__
#define __SAU_CSR_FIXTURE_HH__

#include <string>
#include <vector>

#include "sau/csr_config.hh"

namespace gem5::sau
{

/** Load real csr_writes.csv rows, then replay them through SauCsrConfig. */
std::vector<ReplayedSauCommand>
loadCsrFixture(const std::string &fixtureDirectory,
               const RtlTimingParameters &rtl);

} // namespace gem5::sau

#endif // __SAU_CSR_FIXTURE_HH__
