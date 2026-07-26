#ifndef __SAU_BOUNDARY_TRACE_HH__
#define __SAU_BOUNDARY_TRACE_HH__

#include <fstream>
#include <map>
#include <string>

#include "sau/data_beat.hh"

namespace gem5::sau
{

/**
 * Model-side boundary trace writer for util/sau/compare_boundary.py:
 * ``signal,cycle,value`` rows with per-signal cycle indexes and 0x-hex
 * values (beat byte 31 is the most significant hex byte, matching the
 * RTL bit-slice contract).  Sequence-mode comparison ignores the
 * synthetic cycles; they only need to be nondecreasing per signal.
 */
class BoundaryTraceWriter
{
  public:
    explicit BoundaryTraceWriter(const std::string &path);

    void emit(const std::string &signal, const MemoryBeat256 &value);
    void emitZero(const std::string &signal);
    bool good() const;

  private:
    void writeRow(const std::string &signal, const std::string &value);

    std::ofstream output;
    std::map<std::string, uint64_t> nextCycle;
};

/**
 * Generate the model ABTD B -> B^T boundary trace from the Step 0
 * functional_ref package: the initial memory image feeds the resident
 * and streamed raw-counter address programs, the streamed beats run
 * through the feeder operand-B pipeline into the transposer arbiter,
 * and the emitted trace carries ``sau_sram_rdata``, ``data_B``,
 * ``u_trans2sa_top.trans0_inRow``, and ``u_trans2sa_top.trans0_outCol``
 * for compare_boundary.py.  Returns false when the package image is
 * not present.
 */
bool generateAbtdBoundaryTrace(const std::string &packageDirectory,
                               const std::string &outputPath);

} // namespace gem5::sau

#endif // __SAU_BOUNDARY_TRACE_HH__
