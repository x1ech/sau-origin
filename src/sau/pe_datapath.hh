#ifndef __SAU_PE_DATAPATH_HH__
#define __SAU_PE_DATAPATH_HH__

#include <cstdint>

#include "sau/data_beat.hh"

namespace gem5::sau
{

/**
 * Inputs already aligned to SA_PE.sv's accumulator-update edge.
 *
 * The surrounding array resource owns the RTL multiplication pipeline and
 * column-wstrb delay.  At the PE boundary, SA_PE updates only when both
 * mac_add_en_p3_i and en_p1_i are asserted; synchronous clear has priority
 * over that update.
 */
struct SystolicPeInputs
{
    int8_t activation = 0;
    int8_t weight = 0;
    bool columnEnable = false;
    bool macAddEnable = false;
    bool clear = false;
};

/**
 * PLAN3 Step 4 signed-int8 PE accumulator resource.
 *
 * This class models the architecture-visible state in one SA_PE: a signed
 * 24-bit saturating accumulator.  It deliberately does not own array-level
 * wavefront or pipeline timing; the 32x32 array resource will align the
 * inputs before calling tick().
 */
class SystolicPe
{
  public:
    /** Apply one aligned accumulator edge. */
    void tick(const SystolicPeInputs &inputs);

    /** Asynchronous-reset architectural result. */
    void reset() { accumulatorValue = 0; }

    int32_t accumulator() const { return accumulatorValue; }

    /** SA_ENGINE/SA_pkg int8 quantization of the current accumulator. */
    int8_t quantized(unsigned cutbit) const
    {
        return satTruncateInt24(accumulatorValue, cutbit);
    }

  private:
    int32_t accumulatorValue = 0;
};

} // namespace gem5::sau

#endif // __SAU_PE_DATAPATH_HH__
