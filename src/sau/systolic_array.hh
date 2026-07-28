#ifndef __SAU_SYSTOLIC_ARRAY_HH__
#define __SAU_SYSTOLIC_ARRAY_HH__

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include "sau/data_beat.hh"
#include "sau/pe_datapath.hh"

namespace gem5::sau
{

struct SystolicArrayInput
{
    OperandVector32x8 activations;
    OperandVector32x8 weights;
    /** This is the final accepted vector pair of the accumulation. */
    bool finish = false;
};

/**
 * PLAN3 Step 4 fixed 32x32 signed-int8 systolic-array resource.
 *
 * The frozen RTL builds the array from 8x8 macro blocks of 4x4 PEs.
 * Activation groups descend the macro rows and weight/wstrb groups advance
 * across the macro columns.  One vector pair may enter per cycle; macro block
 * (r, c) receives it after CalcDelay + r + c cycles.
 *
 * Matrix rows use the physical SA_ROW snapshot order.  The source hierarchy
 * reverses the 32 activation lanes: physical row r consumes activation lane
 * 31-r.  Matrix columns preserve the external weight-lane order.
 *
 * Result serialization and the finish/snapshot handshake are separate
 * resources.  Callers may inspect accumulators while bringing up the array,
 * but a stable complete matrix requires pipelineEmpty().
 */
class SystolicArray
{
  public:
    static constexpr unsigned Rows = 32;
    static constexpr unsigned Columns = 32;
    static constexpr unsigned MacroSize = 4;
    static constexpr unsigned MacroRows = Rows / MacroSize;
    static constexpr unsigned MacroColumns = Columns / MacroSize;
    static constexpr unsigned CalcDelay = 3;
    static constexpr unsigned MaxWavefrontDelay =
        CalcDelay + MacroRows - 1 + MacroColumns - 1;
    static constexpr unsigned PipelineSlots = MaxWavefrontDelay + 1;

    using AccumulatorMatrix =
        std::array<std::array<int32_t, Columns>, Rows>;

    /**
     * Advance one array cycle and optionally accept one vector pair.
     * Ready macro events commit before the new pair enters the pipeline.
     */
    void tick(const std::optional<SystolicArrayInput> &input = std::nullopt);

    /**
     * Assert SA_ENGINE.Flag_o until macro row 0 accepts the output token.
     * The stream begins when that row's snapshot is ready.
     */
    void requestOutput(unsigned cutbit);

    /** Reset all PE state and discard every in-flight macro event. */
    void reset();

    /**
     * Configure the current command's SA_ENGINE keep bit. A retaining
     * command snapshots completed tiles without clearing the PE MACs.
     */
    void setKeepMode(bool value) { keepMode = value; }

    /**
     * Start a new command from retained PE state while clearing all
     * command-local pipelines, result snapshots, and stream controls.
     */
    void restoreAccumulators(const AccumulatorMatrix &state);

    bool pipelineEmpty() const { return pendingEvents == 0; }
    uint64_t cycle() const { return currentCycle; }
    uint64_t acceptedInputs() const { return acceptedInputCount; }
    uint64_t committedMacs() const { return committedMacCount; }
    unsigned pendingMacroBlocks() const { return pendingEvents; }
    uint8_t peFinishPulses() const { return peFinishMask; }
    uint8_t snapshotReadyPulses() const { return snapshotReadyMask; }
    bool storageReady() const { return storageReadyValue; }
    bool calFinish() const { return calFinishPulse; }
    const std::optional<OperandVector32x8> &streamOutput() const
    {
        return streamOutputValue;
    }
    std::optional<unsigned> streamRow() const { return streamRowValue; }

    int32_t accumulator(unsigned row, unsigned column) const;
    AccumulatorMatrix accumulators() const;
    OperandVector32x8 quantizedRow(unsigned row, unsigned cutbit) const;

  private:
    struct MacroEvent
    {
        unsigned macroRow = 0;
        unsigned macroColumn = 0;
        std::array<int8_t, MacroSize> activations{};
        std::array<int8_t, MacroSize> weights{};
        bool finish = false;
    };

    enum class StreamState : uint8_t
    {
        Idle,
        WaitToken,
        Streaming
    };

    void commit(const MacroEvent &event);
    void advanceResultStream(uint8_t priorSnapshotReady);

    std::array<std::array<SystolicPe, Columns>, Rows> pes;
    AccumulatorMatrix snapshot{};
    std::array<std::vector<MacroEvent>, PipelineSlots> pipeline;
    std::array<StreamState, MacroRows> streamStates{};
    std::array<unsigned, MacroRows> streamCounters{};
    uint64_t currentCycle = 0;
    uint64_t acceptedInputCount = 0;
    uint64_t committedMacCount = 0;
    unsigned pendingEvents = 0;
    uint8_t peFinishMask = 0;
    uint8_t snapshotReadyMask = 0;
    bool storageReadyValue = false;
    bool calFinishPulse = false;
    bool outputStartActive = false;
    unsigned streamCutbit = 0;
    std::optional<OperandVector32x8> streamOutputValue;
    std::optional<unsigned> streamRowValue;
    bool keepMode = false;
};

} // namespace gem5::sau

#endif // __SAU_SYSTOLIC_ARRAY_HH__
