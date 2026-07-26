#ifndef __SAU_TRANSPOSER_HH__
#define __SAU_TRANSPOSER_HH__

#include <array>
#include <cstdint>

#include "sau/data_beat.hh"
#include "sau/resource_config.hh"

namespace gem5::sau
{

/**
 * transposer_tiny.v bank resource, from the frozen payload source
 * recorded in RTL_TIMING_PROVENANCE.md (Step 3/4 freeze).  One bank
 * holds DIM_R=32 rows of one 32-byte beat.  Accepted transactions move
 * payload and occupancy together:
 *
 * - input side: writeRow() stores at cnt_in; accepting the final row
 *   drops the input ready and raises the output ready.  Writing while
 *   not ready still stores (as the source does) but latches the error
 *   flag.
 * - output side: readOutput() returns one column in transpose mode —
 *   output lane a takes row (31-a), byte (column index), the mapping
 *   proven against the ABTD golden's first prefetched trans0_outCol —
 *   or one row in passthrough mode.  Consuming the final output
 *   restores the input ready; with reuse enabled the output side stays
 *   ready so the bank contents replay, otherwise it closes.
 * - clear() resets counters, flags, and the storage itself (the source
 *   zeroes pe_outL on clear_i).
 *
 * The registered outCol/valid/last delays, same-cycle input bypass, and
 * prefetch-ahead index are cycle-level concerns owned by the strict
 * per-tick driver; this resource models the accepted payload sequence
 * and occupancy.
 */
class TransposerTinyBank
{
  public:
    static constexpr unsigned Rows = 32;

    TransposerTinyBank(bool transposeEnabled, bool reuseEnabled);

    bool inputReady() const;
    bool outputReady() const;
    bool error() const;
    unsigned rowsAccepted() const;
    unsigned outputsTaken() const;

    void writeRow(const MemoryBeat256 &row);

    struct Output
    {
        MemoryBeat256 data;
        bool last = false;
    };
    Output readOutput();

    void clear();

  private:
    MemoryBeat256 column(unsigned index) const;

    const bool transposeEnabled;
    const bool reuseEnabled;
    std::array<MemoryBeat256, Rows> storage{};
    unsigned cntIn = 0;
    unsigned cntOut = 0;
    bool dataInReady = true;
    bool outputReadyFlag = false;
    bool errorFlag = false;
};

/**
 * sa_feeder.sv SA data arbiter routing: which SA port consumes the
 * operand-bank columns for a given input_switch value.  ATBD/ABTD set
 * transposer_work_flag; switch 01 feeds the left (A-side) port from
 * the banks with B direct above, switch 10 feeds the above (B-side)
 * port from the banks with A direct left.
 */
struct SaOperandRouting
{
    bool leftFromTransposer = false;
    bool aboveFromTransposer = false;
};

SaOperandRouting saOperandRouting(SauTransMode mode, uint8_t inputSwitch);

/**
 * sa_feeder.sv operand transposer arbiter over the T0/T1 banks.  From
 * the frozen source: the loading operand is A for ATBD, B for ABTD;
 * ABD without conv is the pure-output mode with input steering
 * disabled.  An accepted row goes to T0 when it can take input,
 * otherwise to T1, and trans_loaded_bank_q remembers the last loaded
 * bank.  At an output-phase start the output bank latches the loaded
 * bank; otherwise it follows whichever bank is exclusively ready.  A
 * command start clears the banks and the selection state unless
 * sa_flow_mode retains them.  T2 belongs to the result serializer and
 * stays outside this arbiter.
 *
 * Within one modeled cycle, present input acceptances before output
 * consumptions: the RTL steering samples the registered pre-edge bank
 * ready, so a same-cycle drain completion must not redirect that
 * cycle's input row.
 */
class TransposerArbiter
{
  public:
    explicit TransposerArbiter(
        const SauTransposeReuseResourceConfig &config);

    bool loadsOperandA() const;
    bool loadsOperandB() const;
    /** Command-start clear; retain modes keep banks and selection. */
    void clearOnStart();
    bool canAcceptRow() const;
    /** Steer one accepted operand row; returns the bank index used. */
    unsigned acceptRow(const MemoryBeat256 &row);
    /** True when the currently selected output bank is drainable. */
    bool columnReady() const;
    /** Output-phase start: latch the output bank from the loaded bank. */
    void startOutputPhase();
    TransposerTinyBank::Output readColumn();
    unsigned outputBank() const;
    const TransposerTinyBank &bank(unsigned index) const;

  private:
    TransposerTinyBank &selectedOutputBank();
    void followExclusiveReady();

    const SauTransposeReuseResourceConfig config;
    const bool pureOutput;
    TransposerTinyBank bank0;
    TransposerTinyBank bank1;
    bool loadedBank = false;
    bool outputBankSel = false;
};

} // namespace gem5::sau

#endif // __SAU_TRANSPOSER_HH__
