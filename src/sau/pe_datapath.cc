#include "sau/pe_datapath.hh"

#include <stdexcept>

namespace gem5::sau
{

void
SystolicPe::restore(int32_t value)
{
    if (value < Int24Min || value > Int24Max) {
        throw std::invalid_argument(
            "retained SAU PE accumulator exceeds signed 24-bit range");
    }
    accumulatorValue = value;
}

void
SystolicPe::tick(const SystolicPeInputs &inputs)
{
    // FFLARNC in SA_PE.sv gives synchronous clear priority over load.
    if (inputs.clear) {
        accumulatorValue = 0;
        return;
    }
    // SA_PE.mac_add_en_p3 is mac_add_en_p3_i & en_p1_i.
    if (!inputs.macAddEnable || !inputs.columnEnable) {
        return;
    }

    const int32_t product =
        static_cast<int32_t>(inputs.activation) *
        static_cast<int32_t>(inputs.weight);
    accumulatorValue = saturateAddInt24(accumulatorValue, product);
}

} // namespace gem5::sau
