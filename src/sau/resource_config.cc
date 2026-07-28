#include "sau/resource_config.hh"

namespace gem5::sau
{

SauResourceConfigs
deriveResourceConfigs(const SauControlFields &control)
{
    SauResourceConfigs configs;

    configs.controller.transMode = control.trans();
    configs.controller.reuseMode = control.reuse();
    configs.controller.saFlowMode = control.saFlow();
    configs.controller.flowTimes = control.flowLoopTimes;
    configs.controller.instructionTimes = control.vertical.instructionCycle;

    configs.streamAddress.baseAddress = control.verticalAddress;
    configs.streamAddress.counters = control.vertical;
    configs.streamAddress.convKernal = control.convKernal;

    configs.residentAddress.baseAddress = control.horizontalAddress;
    configs.residentAddress.xBurst = control.registerInput.xBurst;
    configs.residentAddress.yStep = control.registerInput.yStep;
    configs.residentAddress.yCycle = control.registerInput.yCycle;
    configs.residentAddress.cStep = control.registerInput.cStep;
    configs.residentAddress.cCycle = control.registerInput.cCycle;
    configs.residentAddress.padding = control.registerInput.padding;
    configs.residentAddress.validYStart = control.registerInput.validYStart;
    configs.residentAddress.validYEnd = control.registerInput.validYEnd;
    configs.residentAddress.validXStart = control.registerInput.validXStart;
    configs.residentAddress.validXEnd = control.registerInput.validXEnd;

    configs.input.registerMode = control.registerMode;
    configs.input.padding = control.registerInput.padding;
    configs.input.validYStart = control.registerInput.validYStart;
    configs.input.validYEnd = control.registerInput.validYEnd;
    configs.input.validXStart = control.registerInput.validXStart;
    configs.input.validXEnd = control.registerInput.validXEnd;
    configs.input.streamedCounters = control.input;

    configs.transposeReuse.transMode = control.trans();
    configs.transposeReuse.loadOperandA =
        control.trans() == SauTransMode::ATBD;
    configs.transposeReuse.loadOperandB =
        control.trans() == SauTransMode::ABTD;
    configs.transposeReuse.reuseA = (control.reuseMode & 0x1) != 0;
    configs.transposeReuse.reuseB = (control.reuseMode & 0x2) != 0;
    configs.transposeReuse.retainBanks = (control.saFlowMode & 0x2) != 0;

    configs.array.keepMode = (control.saFlowMode & 0x2) != 0;
    configs.array.cutbit = control.cutbit;

    configs.output.accumulateExisting = (control.saFlowMode & 0x2) != 0;
    configs.output.transposedOrder =
        control.saFlow() == SauSaFlowMode::CTrans;
    configs.output.internalXStep = control.output.xStep;
    configs.output.internalXBurst = control.output.xBurst;
    configs.output.internalYStep = control.output.yStep;
    configs.output.internalYBurst = control.output.yBurst;
    configs.output.internalFlowStep = control.output.flowStep;
    configs.output.internalFlowBurst = control.output.flowBurst;
    configs.output.internalInstructionStep =
        control.output.instructionStep;
    configs.output.internalInstructionBurst =
        control.output.instructionBurst;
    configs.output.registerXBurst = control.output.registerXBurst;
    configs.output.registerYStep = control.output.registerYStep;
    configs.output.registerYCycle = control.output.registerYCycle;
    configs.output.registerCStep = control.output.registerCStep;
    configs.output.registerCCycle = control.output.registerCCycle;

    configs.writeback.baseAddress = control.outputAddress;
    configs.writeback.xStep = control.output.xStep;
    configs.writeback.xBurst = control.output.xBurst;
    configs.writeback.yStep = control.output.yStep;
    configs.writeback.yBurst = control.output.yBurst;
    configs.writeback.flowStep = control.output.flowStep;
    configs.writeback.flowBurst = control.output.flowBurst;
    configs.writeback.instructionStep = control.output.instructionStep;
    configs.writeback.instructionBurst = control.output.instructionBurst;

    return configs;
}

SauRtlPathSelection
selectRtlPaths(const SauControlFields &control)
{
    static const char *const transPaths[] = {
        "T-ABD", "T-ATBD", "T-ABTD", "T-ABDT"
    };
    static const char *const reusePaths[] = {
        "R-none", "R-A", "R-B", "R-AB"
    };
    static const char *const flowPaths[] = {
        "F-normal", "F-trans", "F-retain", "F-tretain"
    };

    SauRtlPathSelection selection;
    selection.transPath = transPaths[control.transMode & 0x3];
    selection.reusePath = reusePaths[control.reuseMode & 0x3];
    selection.flowPath = flowPaths[control.saFlowMode & 0x3];
    return selection;
}

} // namespace gem5::sau
