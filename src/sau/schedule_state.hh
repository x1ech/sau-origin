#ifndef __SAU_SCHEDULE_STATE_HH__
#define __SAU_SCHEDULE_STATE_HH__

#include <cstdint>

#include "sau/types.hh"

namespace gem5::sau
{

// Architecture-level states for the supported ATB/reuse-A path. These are
// intentionally not copies of RTL FSM encodings.
enum class SauScheduleState
{
    Idle,
    ResidentLoad,
    TransposeSetup,
    FlowExecute,
    FlowBoundary,
    DrainAndWriteback,
    Complete,
};

struct SauScheduleStateMapping
{
    SauScheduleState state;
    const char *rtlStates;
    const char *guard;
};

const SauScheduleStateMapping &
scheduleStateMapping(SauScheduleState state);

// Timing-relevant RTL states. The C++ enum does not copy the SystemVerilog
// encoding; it gives the per-tick skeleton names for the source states whose
// guards affect observable timing.
enum class RtlCoreState
{
    Idle,
    RegisterLoad,
    TransposeLoad,
    TransposeClip,
    FirstLoad,
    ReuseLoad,
    DOut,
    RegisterUnload,
};

enum class RtlInstructionState
{
    Idle,
    First,
    Loop,
    Done,
};

struct RtlSchedulerConfig
{
    uint32_t saSize = 32;
    uint32_t flowTimes = 0;
    uint32_t instructionTimes = 0;
    uint8_t transMode = 0;
    uint8_t reuseMode = 0;
    uint8_t saFlowMode = 0;
    bool shiftMode = false;
};

struct RtlSchedulerInputs
{
    // Models the CSR register-6 start write. start itself is an internal
    // one-cycle register, matching csr.sv and scheduler.ins_valid.
    bool startWrite = false;
    bool loadDone = false;
    bool registerLoadDone = false;
    bool updateFinished = false;
    bool writeFinished = false;
    bool lastFlowTimeClear = false;
};

struct RtlResidentLoadConfig
{
    uint32_t xBurst = 0;
    uint32_t yCycles = 0;
    uint32_t channelCycles = 0;
};

struct RtlStreamLoadConfig
{
    uint32_t xBurst = 0;
    uint32_t yCycles = 0;
    uint32_t flowCycles = 0;
    uint32_t instructionCycles = 0;
};

struct RtlStreamLoadInputs
{
    bool start = false;
    RtlCoreState coreState = RtlCoreState::Idle;
    uint8_t inputSwitch = 0;
    bool lastFlowTime = false;
    bool nextExecuteStart = false;
    bool registerRequestValid = false;
    bool registerRequestLast = false;
};

struct RtlStreamLoadOutputs
{
    bool readEnable = false;
    bool readLast = false;
    bool loadDone = false;
};

struct RtlExecuteUpdateConfig
{
    uint32_t saSize = 32;
    uint32_t flowLoops = 0;
    uint8_t convolutionKernel = 0;
    bool keepMode = false;
};

struct RtlExecuteUpdateInputs
{
    bool enable = false;
    bool currentInstructionOutput = false;
    bool resultLast = false;
};

/**
 * Timing-only copy of register_addr.sv's resident x/y/channel counters and
 * registered request valid/last outputs. Addresses and SRAM data are owned by
 * the existing address/storage path; this class only produces scheduler
 * timing signals from CSR extents.
 */
class RtlResidentLoadSkeleton
{
  public:
    explicit RtlResidentLoadSkeleton(const RtlResidentLoadConfig &config);

    void tick(bool start);

    bool requestValid() const { return requestValidReg; }
    bool requestLast() const { return requestLastReg; }

  private:
    enum class State
    {
        Idle,
        Running,
        Done,
    };

    RtlResidentLoadConfig config;
    State state = State::Idle;
    uint32_t xCounter = 0;
    uint32_t yCounter = 0;
    uint32_t channelCounter = 0;
    bool requestValidReg = false;
    bool requestLastReg = false;
};

/**
 * Timing-only copy of mem_addr.sv's streamed vertical address generator for
 * the currently validated matmul layout (one x burst and 32 y cycles).
 * outputs() exposes the RTL combinational load_done together with registered
 * read valid/last; tick() then commits the address FSM and delay registers.
 */
class RtlStreamLoadSkeleton
{
  public:
    explicit RtlStreamLoadSkeleton(const RtlStreamLoadConfig &config);

    RtlStreamLoadOutputs outputs(const RtlStreamLoadInputs &inputs) const;
    void tick(const RtlStreamLoadInputs &inputs);

  private:
    enum class State
    {
        Idle,
        WaitTrigger,
        Running,
        Done,
    };

    enum class Step
    {
        Idle,
        Flow,
        Instruction,
    };

    bool testCounterClear() const;
    bool verticalCounterStart(const RtlStreamLoadInputs &inputs) const;

    RtlStreamLoadConfig config;
    State state = State::Idle;
    Step step = Step::Idle;
    RtlCoreState coreStateDelay = RtlCoreState::Idle;
    uint32_t xCounter = 0;
    uint32_t yCounter = 0;
    uint32_t flowCounter = 0;
    uint32_t instructionCounter = 0;
    uint32_t testCounter = 0;
    bool verticalValid = false;
    bool verticalLast = false;
    bool loadStartDelay = false;
    bool readLastDelay = false;
    bool lastFlowTimeDelay = false;
    bool testCounterValid = false;
    bool readEnableReg = false;
    bool readLastReg = false;
};

/**
 * Timing-only copy of the normal-int8 SA execution counter and sa_feeder's
 * update FSM. It counts accepted SA enable edges, not elapsed wall-clock
 * cycles, and retains the explicit finish/update register chain.
 */
class RtlExecuteUpdateSkeleton
{
  public:
    explicit RtlExecuteUpdateSkeleton(
        const RtlExecuteUpdateConfig &config);

    void tick(const RtlExecuteUpdateInputs &inputs = {});

    uint32_t calculationCount() const { return calculationCounter; }
    bool internalFinish() const { return internalFinishReg; }
    bool executeDone() const { return arrayFinishReg; }
    bool updateFinished() const { return updateFinishedReg; }

  private:
    enum class UpdateState
    {
        FirstOut,
        PingPong,
        LastPingPong,
    };

    RtlExecuteUpdateConfig config;
    uint32_t calculationCycles = 0;
    uint32_t calculationCounter = 0;
    bool internalFinishReg = false;
    bool finishDelayReg = false;
    bool arrayFinishReg = false;
    bool updateFinishedReg = false;
    UpdateState updateState = UpdateState::FirstOut;
};

/**
 * Per-positive-edge control skeleton for scheduler.sv's supported timing
 * path. tick() evaluates guards from the old state and commits every register
 * together, preserving nonblocking-assignment semantics.
 *
 * This component deliberately has no address, data, or precomputed end-cycle
 * input. Later Step 5.5 components will drive its valid/last pulses.
 */
class RtlSchedulerSkeleton
{
  public:
    explicit RtlSchedulerSkeleton(const RtlSchedulerConfig &config);

    void tick(const RtlSchedulerInputs &inputs = {});

    bool start() const { return startReg; }
    RtlCoreState coreState() const { return core; }
    RtlInstructionState instructionState() const { return instruction; }
    uint8_t inputSwitch() const { return inputSwitchReg; }
    uint32_t transposeCount() const { return transposeCounter; }
    uint32_t flowCount() const { return flowCounter; }
    uint32_t instructionCount() const { return instructionCounter; }
    bool updateFinishedLatched() const { return updateFinishedQ; }
    bool nextExecuteStart() const { return nextExecuteStartReg; }
    bool lastFlowTime() const { return lastFlowTimeReg; }
    bool lastInstructionTime() const { return lastInstructionTimeReg; }
    bool lastInstruction() const { return lastInstructionReg; }
    bool commandDone() const { return crossbarDoneReg; }

  private:
    RtlSchedulerConfig config;

    bool startReg = false;
    bool instructionValid = false;
    bool dataLastD = false;
    bool updateFinishedQ = false;
    bool nextExecuteStartReg = false;
    bool lastFlowTimeValidReg = false;
    bool lastFlowTimeReg = false;
    bool lastInstructionTimeReg = false;
    bool lastInstructionTimeClearReg = false;
    bool lastInstructionReg = false;
    bool crossbarDoneReg = false;
    uint8_t inputSwitchReg = 0;
    uint32_t transposeCounter = 0;
    uint32_t flowCounter = 0;
    uint32_t instructionCounter = 0;
    RtlCoreState core = RtlCoreState::Idle;
    RtlInstructionState instruction = RtlInstructionState::Idle;
};

class SauSchedule
{
  public:
    SauScheduleState state() const;

    void beginCommand();
    void completeCommand();

    bool canIssueRead(StreamKind stream) const;
    bool canLoadResident() const;
    bool canAdmitArrayA() const;
    bool canAdmitArrayB() const;
    bool canReleaseResult() const;
    bool canIssueWriteback() const;

    // Call only after the matching real action succeeds. False means that
    // the caller attempted an illegal semantic transition.
    bool onAReadAccepted(bool lastForInstruction);
    bool onResidentReady();
    bool onTransposeComplete();
    bool onArrayAAdmitted();
    bool onArrayBAdmitted(bool flowBoundary, bool finalWork);

  private:
    SauScheduleState current = SauScheduleState::Idle;
    bool residentReady = false;
};

} // namespace gem5::sau

#endif // __SAU_SCHEDULE_STATE_HH__
