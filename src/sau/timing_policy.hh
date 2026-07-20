#ifndef __SAU_TIMING_POLICY_HH__
#define __SAU_TIMING_POLICY_HH__

#include <cstdint>
#include <string>
#include <vector>

#include "sau/types.hh"

namespace gem5::sau
{

/**
 * Fixed elaboration parameters that are observable at the supported RTL
 * boundary.  They are deliberately named after their RTL sources instead of
 * exposing a calibrated-profile delay list.
 */
struct RtlTimingParameters
{
    uint32_t saSize = 32;
    uint32_t registerDepth = 256;
    uint32_t sramDelay = 3;
    uint32_t sramDataWidthBits = 256;
    uint32_t memAddressDelay = 2;
    uint32_t memCtrlDelay = 2;
    uint32_t registerFileAddressDelay = 1;
    uint32_t registerDelay = 2;
    // Fixed sequential stages on the scheduler-to-feeder input-switch path.
    // These are structural registers in feeder.sv, not fixture calibration.
    uint32_t schedulerStateRegisterDelay = 1;
    uint32_t feederInputSwitchRegisterDelay = 1;
    uint32_t feederOutputSwitchRegisterDelay = 1;
    uint32_t resultLastRegisterDelay = 1;
    uint32_t schedulerRegisterUnloadSwitchDelay = 1;
};

/**
 * Observable contract of the single external SRAM request interface used by
 * the supported RTL path.  This deliberately excludes SRAM bank internals,
 * retry, and contention; those remain properties of non-strict system-memory
 * runs.
 */
struct RtlStorageTiming
{
    uint32_t beatBytes = 0;
    uint32_t issueWidth = 0;
    bool sharedReadWritePort = false;
    bool readPriority = false;
    bool inOrderResponses = false;
    Cycles readVisibleLatencyCycles = Cycles(0);

    static RtlStorageTiming derive(const RtlTimingParameters &rtl);
};

struct TimingLedgerEntry
{
    std::string term;
    Cycles cycles;
    std::string source;
};

/**
 * Timing derived for one accepted ATB/reuse-A command.  The policy contains
 * only values consumed by the behavioral scheduler; its ledger retains the
 * structural derivation so a strict trace never depends on an unnamed delay.
 */
struct TimingPolicy
{
    uint8_t transMode = 0;
    uint8_t reuseMode = 0;
    uint32_t residentLoadBeats = 0;
    uint32_t inputBeatsPerInstruction = 0;
    uint32_t outputBeats = 0;
    uint32_t arrayInputABeats = 0;
    uint32_t arrayInputBBeats = 0;
    uint32_t scheduleInstructions = 0;
    bool shortDirectDOutPath = false;
    bool earlyFinalUnload = false;
    RtlStorageTiming storage;

    Cycles commandStartCycles = Cycles(0);
    Cycles arrayInputStartDelayCycles = Cycles(0);
    uint32_t arrayInputBurstBeats = 0;
    Cycles arrayInputBurstGapCycles = Cycles(0);
    Cycles arrayInputFlowGapCycles = Cycles(0);
    Cycles firstArrayInputFlowGapCycles = Cycles(0);
    Cycles secondArrayInputFlowGapCycles = Cycles(0);
    Cycles steadyArrayInputFlowGapCycles = Cycles(0);
    uint32_t arrayInputSkewCycles = 0;
    uint32_t bReadStartAheadBeats = 0;
    uint32_t bStagingBeats = 0;
    Cycles arrayFillCycles = Cycles(0);
    Cycles inputSwitchVisibleDelayCycles = Cycles(0);
    Cycles inputSwitchResetVisibleDelayCycles = Cycles(0);
    Cycles flowExecuteCycles = Cycles(0);
    Cycles resultFlowGapCycles = Cycles(0);
    Cycles firstShortExecuteCycles = Cycles(0);
    Cycles steadyShortExecuteCycles = Cycles(0);
    Cycles firstShortDrainCycles = Cycles(0);
    Cycles secondShortDrainCycles = Cycles(0);
    Cycles steadyShortDrainCycles = Cycles(0);
    Cycles finalDrainToInputSwitchResetCycles = Cycles(0);
    Cycles writebackStartDelayCycles = Cycles(0);
    Cycles earlyUnloadWritebackStartDelayCycles = Cycles(0);
    Cycles completionDelayCycles = Cycles(0);
    std::vector<TimingLedgerEntry> ledger;

    static TimingPolicy derive(const SauCommand &command, uint8_t transMode,
                               uint8_t reuseMode,
                               const RtlTimingParameters &rtl);

    Cycles arrayInputFlowGapAfter(uint32_t completedFlows) const;
    Cycles shortExecuteCycles(uint32_t instructionIndex) const;
    Cycles shortDrainCycles(uint32_t instructionIndex) const;
};

} // namespace gem5::sau

#endif // __SAU_TIMING_POLICY_HH__
