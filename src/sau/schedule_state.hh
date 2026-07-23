#ifndef __SAU_SCHEDULE_STATE_HH__
#define __SAU_SCHEDULE_STATE_HH__

#include <cstdint>
#include <vector>

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

struct RtlSaEnableInputs
{
    bool dataAValid = false;
    bool dataBValid = false;
    uint8_t inputSwitch = 0;
};

struct RtlResultSerializerConfig
{
    uint32_t saSize = 32;
    uint32_t peRowNum = 4;
    uint32_t peColNum = 4;
};

struct RtlResultSerializerInputs
{
    bool internalFinish = false;
};

struct RtlOutputWritebackConfig
{
    uint32_t internalXBurst = 0;
    uint32_t internalYBurst = 0;
    uint32_t internalFlowBurst = 0;
    uint32_t internalInstructionBurst = 0;
    uint32_t outputXBurst = 0;
    uint32_t outputYCycles = 0;
    uint32_t outputCCycles = 0;
};

struct RtlOutputWritebackInputs
{
    bool resultValid = false;
    RtlCoreState coreState = RtlCoreState::Idle;
};

struct RtlSramWriteTransportInputs
{
    bool nativeWriteValid = false;
    bool nativeWriteLast = false;
    bool crossbarStart = false;
    bool crossbarDone = false;
    bool dbusRequest = false;
};

struct RtlResidentFillConfig
{
    uint32_t sramDelay = 3;
};

struct RtlResidentFillInputs
{
    bool readRequestValid = false;
    bool readRequestLast = false;
    RtlCoreState coreState = RtlCoreState::Idle;
};

struct RtlInputFeederConfig
{
    uint32_t xBurst = 0;
    uint32_t yBurst = 0;
    uint32_t flowBurst = 0;
    uint32_t instructionBurst = 0;
    uint32_t saSize = 32;
    uint32_t sramDelay = 3;
    uint32_t addressDelay = 2;
    uint32_t memoryControlDelay = 2;
    uint32_t registerDelay = 2;
};

struct RtlInputFeederInputs
{
    RtlCoreState coreState = RtlCoreState::Idle;
    uint8_t inputSwitch = 0;
    bool memoryDataValid = false;
    bool lastFlowTime = false;
};

struct RtlCommandDriverConfig
{
    RtlSchedulerConfig scheduler;
    RtlResidentLoadConfig residentLoad;
    RtlStreamLoadConfig streamLoad;
    RtlInputFeederConfig inputFeeder;
    RtlExecuteUpdateConfig execute;
    RtlResultSerializerConfig resultSerializer;
    RtlOutputWritebackConfig outputWriteback;
    uint32_t sramDelay = 3;
};

struct RtlStageWindow
{
    bool observed = false;
    uint64_t firstEdge = 0;
    uint64_t lastEdge = 0;

    void observe(uint64_t edge);
    uint64_t span() const;
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
 * Timing-only copy of sa_feeder's EN_i/EN_i_d/sa_en_i boundary. The caller
 * supplies the pre-edge input_switch_i and A/B valids. saEnable() is sampled
 * before tick(), then tick() commits EN_i_d like the RTL positive edge.
 */
class RtlSaEnableSkeleton
{
  public:
    bool saEnable(const RtlSaEnableInputs &inputs) const;
    void tick(const RtlSaEnableInputs &inputs);

    bool inputEnableDelayed() const { return inputEnableDelayReg; }

  private:
    bool inputEnableDelayReg = false;
};

/**
 * Timing-only copy of the supported SA_ENGINE macro-array result path,
 * sa_feeder output-start registers, and output transposer valid/last counters.
 * It transports only control tokens; arithmetic result data is not modeled.
 */
class RtlResultSerializerSkeleton
{
  public:
    explicit RtlResultSerializerSkeleton(
        const RtlResultSerializerConfig &config);

    void tick(const RtlResultSerializerInputs &inputs = {});

    bool firstMacroValid() const { return firstMacroValidReg; }
    bool engineStorageReady() const { return engineStorageReadyReg; }
    bool rowScoreValid() const { return rowScoreValidReg; }
    bool transposerReadyOut() const { return transposerReadyOutReg; }
    bool transposerValid() const { return transposerValidReg; }
    bool transposerLast() const { return transposerLastReg; }
    bool resultValid() const { return resultValidReg; }
    bool resultLast() const { return resultLastReg; }

  private:
    RtlResultSerializerConfig config;
    std::vector<bool> finishToMacroPipeline;
    bool firstMacroValidReg = false;
    bool engineStorageReadyReg = false;
    bool engineStorageReadyDelayReg = false;
    bool outputPendingReg = false;
    bool serialActiveReg = false;
    bool resultGateReg = false;
    bool outputStartReg = false;
    bool macroResultPendingReg = false;
    bool macroStreamingReg = false;
    uint32_t macroStreamCount = 0;
    uint32_t globalRowCount = 0;
    bool rowScoreValidReg = false;
    bool calFinishReg = false;
    bool transposerInputReadyReg = true;
    bool transposerReadyOutReg = false;
    uint32_t transposerInputCount = 0;
    uint32_t transposerOutputCount = 0;
    bool transposerValidReg = false;
    bool transposerLastReg = false;
    bool resultValidReg = false;
    bool resultLastReg = false;
};

/**
 * Timing-only copy of register_file_out.sv's result-accumulation completion,
 * REGISTER_UNLOAD launch, output register_addr instance, and final write-done
 * delay. Result data and addresses remain owned by the existing data path.
 */
class RtlOutputWritebackSkeleton
{
  public:
    explicit RtlOutputWritebackSkeleton(
        const RtlOutputWritebackConfig &config);

    void tick(const RtlOutputWritebackInputs &inputs = {});

    bool resultAccumDone() const { return resultAccumDoneReg; }
    bool writeValid() const { return writeValidDelay2Reg; }
    bool writeDataLast() const { return writeLastDelay2Reg; }
    bool writeFinished() const { return writeDoneDelay4Reg; }

  private:
    RtlOutputWritebackConfig config;
    RtlResidentLoadSkeleton outputAddress;
    uint32_t resultXCounter = 0;
    uint32_t resultYCounter = 0;
    uint32_t resultFlowCounter = 0;
    uint32_t resultInstructionCounter = 0;
    bool resultAccumDoneReg = false;
    bool registerOutStateReg = false;
    bool registerOutStateDelayReg = false;
    bool writeValidDelay1Reg = false;
    bool writeValidDelay2Reg = false;
    bool writeLastDelay1Reg = false;
    bool writeLastDelay2Reg = false;
    bool writeDoneDelay1Reg = false;
    bool writeDoneDelay2Reg = false;
    bool writeDoneDelay3Reg = false;
    bool writeDoneDelay4Reg = false;
};

/**
 * Timing-only copy of the write portion of mem_ctrl.sv, crossbar_mi.sv, and
 * the native-priority shared-memory input. There is no native ready signal:
 * an ACTIVE crossbar forwards every registered request and the SRAM samples
 * it on the following positive edge.
 */
class RtlSramWriteTransportSkeleton
{
  public:
    void tick(const RtlSramWriteTransportInputs &inputs = {});

    bool crossbarActive() const { return state == State::Active; }
    bool memCtrlWriteValid() const { return memCtrlWriteValidReg; }
    bool memCtrlWriteLast() const { return memCtrlWriteLastReg; }
    bool crossbarSlaveValid() const { return crossbarSlaveValidReg; }
    bool memoryWriteAccepted() const { return memoryWriteAcceptedReg; }
    bool memoryWriteLast() const { return memoryWriteLastReg; }

  private:
    enum class State
    {
        Idle,
        Active,
        RvActive,
    };

    State state = State::Idle;
    bool memCtrlWriteValidReg = false;
    bool memCtrlWriteLastReg = false;
    bool crossbarSlaveValidReg = false;
    bool crossbarSlaveLastReg = false;
    bool memoryWriteAcceptedReg = false;
    bool memoryWriteLastReg = false;
};

/**
 * Timing-only copy of the resident-read visibility path through mem_ctrl,
 * feeder.register_file_wvalid_o, and register_file_in's padding-shifter
 * valid register. It intentionally allows the tail to drain after scheduler
 * leaves REGISTER_LOAD, as the delayed core-state pipe does in RTL.
 */
class RtlResidentFillSkeleton
{
  public:
    explicit RtlResidentFillSkeleton(const RtlResidentFillConfig &config);

    void tick(const RtlResidentFillInputs &inputs = {});

    bool memoryDataValid() const { return memoryDataValidReg; }
    bool memoryDataLast() const { return memoryDataLastReg; }
    bool registerFileInputValid() const
    {
        return registerFileInputValidReg;
    }
    bool residentWriteValid() const { return residentWriteValidReg; }

  private:
    RtlResidentFillConfig config;
    std::vector<bool> memoryValidPipeline;
    std::vector<bool> memoryLastPipeline;
    std::vector<RtlCoreState> coreStatePipeline;
    bool memoryDataValidReg = false;
    bool memoryDataLastReg = false;
    bool registerFileInputValidReg = false;
    bool residentWriteValidReg = false;
};

/**
 * Timing-only copy of the fixed ATB/reuse-A input-RF read path and feeder
 * A/B-valid pipelines. It models register_file_in's x/y/flow/instruction
 * read counters, feeder.STATE_DELAY, REGISTER_DELAY=2, and the final A/B and
 * input-switch registers. Data values are intentionally not represented.
 */
class RtlInputFeederSkeleton
{
  public:
    explicit RtlInputFeederSkeleton(const RtlInputFeederConfig &config);

    void tick(const RtlInputFeederInputs &inputs = {});

    bool registerFileReadEnable() const { return readEnableReg; }
    bool registerFileReadValid() const { return readValidReg; }
    bool registerFileReadLast() const { return readLastReg; }
    bool dataAValid() const { return dataAValidReg; }
    bool dataBValid() const { return dataBValidReg; }
    uint8_t outputInputSwitch() const { return outputInputSwitchReg; }
    bool lastFlowTimeClear() const { return lastFlowTimeClearReg; }

  private:
    enum class ReadState
    {
        Idle,
        Burst,
    };

    RtlInputFeederConfig config;
    std::vector<RtlCoreState> coreStatePipeline;
    std::vector<uint8_t> inputSwitchPipeline;
    std::vector<bool> lastFlowTimePipeline;
    std::vector<uint8_t> outputInputSwitchPipeline;
    std::vector<bool> memoryValidPipeline;
    RtlCoreState delayedCoreStateReg = RtlCoreState::Idle;
    bool reuseLoadStateReg = false;
    bool memoryDataValidReg = false;
    bool readEnableReg = false;
    ReadState readState = ReadState::Idle;
    uint32_t xCounter = 0;
    uint32_t yCounter = 0;
    uint32_t flowCounter = 0;
    uint32_t instructionCounter = 0;
    bool readValidReg = false;
    bool readLastReg = false;
    bool shiftDataValidReg = false;
    uint32_t shiftDataCounter = 0;
    bool dataAValidReg = false;
    bool dataBValidReg = false;
    uint8_t inputSwitchDelayReg = 0;
    uint8_t outputInputSwitchReg = 0;
    bool inputSwitchCaseReg = false;
    bool lastFlowTimeClearReg = false;
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

/**
 * Isolated CSR-to-command-done timing driver for the current fixed
 * ATB/reuse-A contract. Every component samples one shared pre-edge snapshot
 * and commits at the end of tick(). The driver contains no golden cycles,
 * fixture identity, matrix dimensions, addresses, or arithmetic data.
 */
class RtlCommandDriverSkeleton
{
  public:
    explicit RtlCommandDriverSkeleton(
        const RtlCommandDriverConfig &config);

    void tick(bool startWrite = false);

    RtlCoreState coreState() const { return scheduler.coreState(); }
    bool commandDone() const { return scheduler.commandDone(); }
    bool streamReadEnable() const
    {
        return currentStreamReadEnable;
    }
    bool memoryDataValid() const { return readPath.memoryDataValid(); }
    bool memoryReadRequestValid() const
    {
        return memoryReadAcceptedReg;
    }
    bool memoryReadRequestLast() const
    {
        return memoryReadAcceptedLastReg;
    }
    bool memoryReadRequestIsStream() const
    {
        return memoryReadAcceptedStreamReg;
    }
    bool registerFileReadEnable() const
    {
        return inputFeeder.registerFileReadEnable();
    }
    bool registerFileReadValid() const
    {
        return inputFeeder.registerFileReadValid();
    }
    bool dataAValid() const { return inputFeeder.dataAValid(); }
    bool dataBValid() const { return inputFeeder.dataBValid(); }
    uint8_t outputInputSwitch() const
    {
        return inputFeeder.outputInputSwitch();
    }
    bool saEnable() const { return currentSaEnable; }
    bool internalFinish() const { return execute.internalFinish(); }
    bool resultValid() const { return resultSerializer.resultValid(); }
    bool resultLast() const { return resultSerializer.resultLast(); }
    bool nativeWriteValid() const { return outputWriteback.writeValid(); }
    bool nativeWriteLast() const
    {
        return outputWriteback.writeDataLast();
    }
    bool memoryWriteRequestValid() const
    {
        return writeTransport.memCtrlWriteValid();
    }
    bool memoryWriteRequestLast() const
    {
        return writeTransport.memCtrlWriteLast();
    }
    bool physicalWriteAccepted() const
    {
        return writeTransport.memoryWriteAccepted();
    }
    bool physicalWriteLast() const
    {
        return writeTransport.memoryWriteLast();
    }

    uint64_t residentReadTokens() const { return residentReads; }
    uint64_t streamReadTokens() const { return streamReads; }
    uint64_t registerFileReadTokens() const { return registerFileReads; }
    uint64_t operandATokens() const { return operandAInputs; }
    uint64_t operandBTokens() const { return operandBInputs; }
    uint64_t acceptedSaTokens() const { return acceptedSaInputs; }
    uint64_t resultTokens() const { return results; }
    uint64_t nativeWriteTokens() const { return nativeWrites; }
    uint64_t physicalWriteTokens() const { return physicalWrites; }
    const RtlStageWindow &residentReadWindow() const
    {
        return residentReadStage;
    }
    const RtlStageWindow &streamReadWindow() const
    {
        return streamReadStage;
    }
    const RtlStageWindow &operandAWindow() const
    {
        return operandAStage;
    }
    const RtlStageWindow &operandBWindow() const
    {
        return operandBStage;
    }
    const RtlStageWindow &resultWindow() const
    {
        return resultStage;
    }
    const RtlStageWindow &memoryWriteWindow() const
    {
        return memoryWriteStage;
    }
    bool commandDoneObserved() const { return commandDoneSeen; }
    uint64_t commandDoneEdge() const { return commandDoneAt; }

  private:
    RtlCommandDriverConfig config;
    RtlSchedulerSkeleton scheduler;
    RtlResidentLoadSkeleton residentLoad;
    RtlStreamLoadSkeleton streamLoad;
    RtlResidentFillSkeleton readPath;
    RtlInputFeederSkeleton inputFeeder;
    RtlSaEnableSkeleton saEnablePath;
    RtlExecuteUpdateSkeleton execute;
    RtlResultSerializerSkeleton resultSerializer;
    RtlOutputWritebackSkeleton outputWriteback;
    RtlSramWriteTransportSkeleton writeTransport;
    bool currentStreamReadEnable = false;
    bool currentSaEnable = false;
    bool memoryReadAcceptedReg = false;
    bool memoryReadAcceptedLastReg = false;
    bool memoryReadAcceptedStreamReg = false;
    uint64_t residentReads = 0;
    uint64_t streamReads = 0;
    uint64_t registerFileReads = 0;
    uint64_t operandAInputs = 0;
    uint64_t operandBInputs = 0;
    uint64_t acceptedSaInputs = 0;
    uint64_t results = 0;
    uint64_t nativeWrites = 0;
    uint64_t physicalWrites = 0;
    uint64_t currentEdge = 0;
    RtlStageWindow residentReadStage;
    RtlStageWindow streamReadStage;
    RtlStageWindow operandAStage;
    RtlStageWindow operandBStage;
    RtlStageWindow resultStage;
    RtlStageWindow memoryWriteStage;
    bool commandDoneSeen = false;
    uint64_t commandDoneAt = 0;
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
