#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "sau/resource_config.hh"

namespace gem5::sau
{
namespace
{

/** The small ATBD/reuse-A coverage fixture's raw control state. */
SauControlFields
atbdReuseAControl()
{
    SauControlFields control;
    control.transMode = 1;
    control.reuseMode = 1;
    control.saFlowMode = 0;
    control.registerMode = 0;
    control.cutbit = 8;
    control.flowLoopTimes = 1;
    control.horizontalAddress = 0x29120000;
    control.verticalAddress = 0x29120400;
    control.outputAddress = 0x29120c00;
    control.input.xBurst = 1;
    control.input.yStep = 1;
    control.input.yBurst = 32;
    control.input.instructionBurst = 1;
    control.vertical.xBurst = 1;
    control.vertical.yStep = 1;
    control.vertical.yCycle = 32;
    control.vertical.flowCycle = 1;
    control.vertical.instructionCycle = 1;
    control.registerInput.xBurst = 1;
    control.registerInput.yStep = 1;
    control.registerInput.yCycle = 32;
    control.registerInput.padding = 0;
    control.output.xBurst = 1;
    control.output.yStep = 1;
    control.output.yBurst = 32;
    control.output.instructionBurst = 1;
    control.output.registerXBurst = 1;
    control.output.registerYCycle = 32;
    return control;
}

TEST(SauResourceConfig, DispatchesOnlyRtlConnectedFieldsPerResource)
{
    const auto configs = deriveResourceConfigs(atbdReuseAControl());

    EXPECT_EQ(configs.controller.transMode, SauTransMode::ATBD);
    EXPECT_EQ(configs.controller.reuseMode, SauReuseMode::ReuseA);
    EXPECT_EQ(configs.controller.saFlowMode, SauSaFlowMode::CNormal);
    EXPECT_EQ(configs.controller.flowTimes, 1);
    EXPECT_EQ(configs.controller.instructionTimes, 1);

    EXPECT_EQ(configs.streamAddress.baseAddress, 0x29120400);
    EXPECT_EQ(configs.streamAddress.counters.yCycle, 32);
    EXPECT_EQ(configs.residentAddress.baseAddress, 0x29120000);
    EXPECT_EQ(configs.residentAddress.xBurst, 1);
    EXPECT_EQ(configs.residentAddress.yCycle, 32);

    EXPECT_EQ(configs.input.registerMode, 0);
    EXPECT_EQ(configs.input.padding, 0);
    EXPECT_EQ(configs.input.streamedCounters.yBurst, 32);

    EXPECT_TRUE(configs.transposeReuse.loadOperandA);
    EXPECT_FALSE(configs.transposeReuse.loadOperandB);
    EXPECT_TRUE(configs.transposeReuse.transposedResult);
    EXPECT_TRUE(configs.transposeReuse.reuseA);
    EXPECT_FALSE(configs.transposeReuse.reuseB);
    EXPECT_FALSE(configs.transposeReuse.retainBanks);

    EXPECT_FALSE(configs.array.keepMode);
    EXPECT_EQ(configs.array.cutbit, 8);

    EXPECT_FALSE(configs.output.accumulateExisting);
    EXPECT_FALSE(configs.output.transposedOrder);
    EXPECT_EQ(configs.output.registerYCycle, 32);

    EXPECT_EQ(configs.writeback.baseAddress, 0x29120c00);
    EXPECT_EQ(configs.writeback.yBurst, 32);
}

TEST(SauResourceConfig, ChangingALegalFieldChangesOnlyItsOwningResource)
{
    const auto baseline = deriveResourceConfigs(atbdReuseAControl());

    // cutbit owns only the array shift/saturation boundary.
    auto control = atbdReuseAControl();
    control.cutbit = 1;
    auto changed = deriveResourceConfigs(control);
    EXPECT_EQ(changed.array.cutbit, 1);
    EXPECT_EQ(changed.transposeReuse.loadOperandA,
              baseline.transposeReuse.loadOperandA);
    EXPECT_EQ(changed.streamAddress.baseAddress,
              baseline.streamAddress.baseAddress);
    EXPECT_EQ(changed.writeback.baseAddress, baseline.writeback.baseAddress);

    // trans_mode owns the transposer bank load side and result transpose.
    control = atbdReuseAControl();
    control.transMode = 2;
    changed = deriveResourceConfigs(control);
    EXPECT_EQ(changed.controller.transMode, SauTransMode::ABTD);
    EXPECT_FALSE(changed.transposeReuse.loadOperandA);
    EXPECT_TRUE(changed.transposeReuse.loadOperandB);
    EXPECT_TRUE(changed.transposeReuse.transposedResult);
    EXPECT_EQ(changed.array.cutbit, baseline.array.cutbit);

    // trans_mode=00 is the only direct-result path.
    control = atbdReuseAControl();
    control.transMode = 0;
    changed = deriveResourceConfigs(control);
    EXPECT_FALSE(changed.transposeReuse.loadOperandA);
    EXPECT_FALSE(changed.transposeReuse.loadOperandB);
    EXPECT_FALSE(changed.transposeReuse.transposedResult);

    // reuse_mode drives the two scheduler reuse bits; 11 asserts both.
    control = atbdReuseAControl();
    control.reuseMode = 3;
    changed = deriveResourceConfigs(control);
    EXPECT_TRUE(changed.transposeReuse.reuseA);
    EXPECT_TRUE(changed.transposeReuse.reuseB);

    // sa_flow_mode bit1 retains banks/accumulators and switches the
    // output RF to accumulate; bit0 selects transposed output order.
    control = atbdReuseAControl();
    control.saFlowMode = 3;
    changed = deriveResourceConfigs(control);
    EXPECT_TRUE(changed.transposeReuse.retainBanks);
    EXPECT_TRUE(changed.array.keepMode);
    EXPECT_TRUE(changed.output.accumulateExisting);
    EXPECT_TRUE(changed.output.transposedOrder);
    EXPECT_EQ(changed.input.padding, baseline.input.padding);

    // A vertical counter change reaches only the stream address program.
    control = atbdReuseAControl();
    control.vertical.yStep = 8;
    changed = deriveResourceConfigs(control);
    EXPECT_EQ(changed.streamAddress.counters.yStep, 8);
    EXPECT_EQ(changed.residentAddress.yStep, baseline.residentAddress.yStep);
    EXPECT_EQ(changed.input.streamedCounters.yStep,
              baseline.input.streamedCounters.yStep);

    // Padding reaches the input resource and the resident address
    // program (register_addr.sv consumes the pad/valid window itself),
    // but nothing else.
    control = atbdReuseAControl();
    control.registerInput.padding = 5;
    changed = deriveResourceConfigs(control);
    EXPECT_EQ(changed.input.padding, 5);
    EXPECT_EQ(changed.residentAddress.padding, 5);
    EXPECT_EQ(changed.array.cutbit, baseline.array.cutbit);

    // Output register-side and mem-side counters reach their own
    // resources independently.
    control = atbdReuseAControl();
    control.output.registerYCycle = 16;
    control.output.xStep = 4;
    changed = deriveResourceConfigs(control);
    EXPECT_EQ(changed.output.registerYCycle, 16);
    EXPECT_EQ(changed.output.internalXStep, 4);
    EXPECT_EQ(changed.output.internalYBurst,
              baseline.output.internalYBurst);
    EXPECT_EQ(changed.writeback.xStep, 4);
    EXPECT_EQ(changed.writeback.yBurst, baseline.writeback.yBurst);
}

TEST(SauResourceConfig, SelectsTheStepZeroPathForEveryLegalCombination)
{
    static const char *const expectedTrans[] = {
        "T-ABD", "T-ATBD", "T-ABTD", "T-ABDT"
    };
    static const char *const expectedReuse[] = {
        "R-none", "R-A", "R-B", "R-AB"
    };
    static const char *const expectedFlow[] = {
        "F-normal", "F-trans", "F-retain", "F-tretain"
    };

    for (uint32_t trans = 0; trans <= 3; ++trans) {
        for (uint32_t reuse = 0; reuse <= 3; ++reuse) {
            for (uint32_t flow = 0; flow <= 3; ++flow) {
                auto control = atbdReuseAControl();
                control.transMode = trans;
                control.reuseMode = reuse;
                control.saFlowMode = flow;

                const auto selection = selectRtlPaths(control);
                EXPECT_STREQ(selection.transPath, expectedTrans[trans]);
                EXPECT_STREQ(selection.reusePath, expectedReuse[reuse]);
                EXPECT_STREQ(selection.flowPath, expectedFlow[flow]);

                // The operand-transposer ownership must stay consistent
                // with the selected structural path row.
                const auto configs = deriveResourceConfigs(control);
                EXPECT_EQ(configs.transposeReuse.loadOperandA, trans == 1);
                EXPECT_EQ(configs.transposeReuse.loadOperandB, trans == 2);
                EXPECT_EQ(configs.transposeReuse.transposedResult,
                          trans != 0);
            }
        }
    }
}

} // anonymous namespace
} // namespace gem5::sau
