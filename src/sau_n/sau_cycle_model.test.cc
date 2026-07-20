#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <stdexcept>

#include "sau_n/sau_model.hh"

namespace gem5::sau_n
{
namespace
{

SauCycleInputs
launchInputs(uint64_t cycles, uint64_t rows, uint64_t columns)
{
    SauCycleInputs inputs;
    inputs.insValid = true;
    inputs.config.calcCycles = cycles;
    inputs.config.validRows = rows;
    inputs.config.validColumns = columns;
    return inputs;
}

SauCycleInputs
streamInputs(int8_t activation, int8_t weight)
{
    SauCycleInputs inputs;
    inputs.inputValid = true;
    inputs.activations.fill(activation);
    inputs.weights.fill(weight);
    return inputs;
}

TEST(SauCycleContract, FreezesStateEncodingAndProvisionalAnchors)
{
    EXPECT_EQ(static_cast<uint8_t>(SauEngineState::Idle), uint8_t{0});
    EXPECT_EQ(static_cast<uint8_t>(SauEngineState::Start), uint8_t{1});
    EXPECT_EQ(static_cast<uint8_t>(SauEngineState::Work), uint8_t{2});
    EXPECT_EQ(static_cast<uint8_t>(SauEngineState::Storage), uint8_t{3});
    EXPECT_EQ(static_cast<uint8_t>(SauEngineState::Done), uint8_t{4});
    EXPECT_EQ(sauEngineStateName(SauEngineState::Storage), "STORAGE");
    EXPECT_EQ(ProvisionalMacCommitDelay, uint64_t{4});
    EXPECT_EQ(ProvisionalBiasCommitDelay, uint64_t{5});
    EXPECT_EQ(ProvisionalRowResultDelay, uint64_t{6});

    SauCycleModel model;
    EXPECT_TRUE(model.cycleAnchorsProvisional());
}

TEST(SauCycleModel, K9OneByOneFollowsCandidatePipeline)
{
    SauCycleModel model;
    auto launch = launchInputs(9, 1, 1);
    auto observation = model.tick(launch);
    EXPECT_EQ(observation.cycle, uint64_t{0});
    EXPECT_EQ(observation.state, SauEngineState::Idle);

    for (uint64_t input = 1; input <= 9; ++input) {
        observation = model.tick(streamInputs(1, 1));
        if (observation.cycle == 5) {
            EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 0));
            EXPECT_EQ(observation.peStates[peIndex(0, 0)].accumulator, 1);
        }
    }
    EXPECT_EQ(observation.cycle, uint64_t{9});
    EXPECT_EQ(observation.state, SauEngineState::Work);
    EXPECT_EQ(observation.dataInCount, uint64_t{8});

    for (uint64_t cycle = 10; cycle <= 15; ++cycle) {
        observation = model.tick();
        if (cycle == 13) {
            EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 0));
            EXPECT_EQ(observation.peStates[peIndex(0, 0)].accumulator, 9);
        }
        if (cycle == 14) {
            EXPECT_TRUE(peMaskBit(observation.addCommitMask, 0, 0));
        }
    }
    EXPECT_EQ(observation.cycle, uint64_t{15});
    EXPECT_TRUE(observation.peFinish);
    EXPECT_EQ(observation.osValidMask, uint16_t{0x0001});
    EXPECT_EQ(observation.state, SauEngineState::Work);
    EXPECT_FALSE(observation.storageReady);

    SauCycleInputs request;
    request.outputRequest = true;
    observation = model.tick(request);
    EXPECT_EQ(observation.cycle, uint64_t{16});
    EXPECT_TRUE(observation.storageReady);
    EXPECT_TRUE(observation.internalOutputValid);
    EXPECT_TRUE(observation.engineOutputFire);

    observation = model.tick();
    EXPECT_EQ(observation.cycle, uint64_t{17});
    EXPECT_TRUE(observation.rowScoreValid);
    EXPECT_EQ(observation.rowSequence, uint64_t{0});
    EXPECT_EQ(observation.outputSlots[0], uint16_t{0x0009});
    EXPECT_TRUE(observation.calFinish);
    EXPECT_EQ(observation.state, SauEngineState::Storage);

    observation = model.tick();
    EXPECT_EQ(observation.cycle, uint64_t{18});
    EXPECT_FALSE(observation.calFinish);
    EXPECT_FALSE(observation.storageReady);
    EXPECT_EQ(observation.state, SauEngineState::Idle);
}

TEST(SauCycleModel, SkewsPeCommitMaskInCanonicalRowMajorOrder)
{
    SauCycleModel model;
    model.tick(launchInputs(9, 2, 3));
    for (uint64_t input = 1; input <= 5; ++input) {
        const auto observation = model.tick(streamInputs(2, -4));
        if (observation.cycle == 5) {
            EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 0));
            EXPECT_FALSE(peMaskBit(observation.macCommitMask, 0, 1));
            EXPECT_FALSE(peMaskBit(observation.macCommitMask, 1, 0));
            EXPECT_EQ(
                observation.peStates[peIndex(0, 0)].activation,
                int8_t{2});
            EXPECT_EQ(
                observation.peStates[peIndex(0, 0)].weight,
                int8_t{-4});
        }
    }

    auto observation = model.tick(streamInputs(2, -4));
    EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 0));
    EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 1));
    EXPECT_TRUE(peMaskBit(observation.macCommitMask, 1, 0));
    EXPECT_FALSE(peMaskBit(observation.macCommitMask, 1, 1));

    observation = model.tick(streamInputs(2, -4));
    EXPECT_TRUE(peMaskBit(observation.macCommitMask, 0, 2));
    EXPECT_TRUE(peMaskBit(observation.macCommitMask, 1, 1));
    EXPECT_FALSE(peMaskBit(observation.macCommitMask, 1, 2));
}

TEST(SauCycleModel, RowColumnTailAndBackpressurePreserveOutputs)
{
    SauCycleModel model;
    auto launch = launchInputs(9, 2, 3);
    launch.config.biases[0] = 1;
    launch.config.biases[1] = -2;
    launch.config.biases[2] = 3;
    model.tick(launch);

    for (uint64_t input = 0; input < 9; ++input) {
        SauCycleInputs stream;
        stream.inputValid = true;
        stream.activations[0] = 2;
        stream.activations[1] = -3;
        stream.weights[0] = -4;
        stream.weights[1] = 5;
        stream.weights[2] = 6;
        model.tick(stream);
    }

    SauCycleObservation observation;
    for (uint64_t cycle = 10; cycle <= 17; ++cycle) {
        observation = model.tick();
    }
    EXPECT_EQ(observation.cycle, uint64_t{17});
    EXPECT_EQ(observation.osValidMask, uint16_t{0x0001});

    SauCycleInputs blocked;
    blocked.outputRequest = true;
    blocked.outputGrant = false;
    observation = model.tick(blocked);
    EXPECT_EQ(observation.cycle, uint64_t{18});
    EXPECT_EQ(observation.osValidMask, uint16_t{0x0002});
    EXPECT_TRUE(observation.storageReady);
    EXPECT_FALSE(observation.engineOutputFire);
    EXPECT_EQ(
        observation.peStates[peIndex(0, 3)].accumulator,
        int32_t{0});
    EXPECT_EQ(
        observation.peStates[peIndex(2, 0)].accumulator,
        int32_t{0});

    observation = model.tick(blocked);
    EXPECT_FALSE(observation.engineOutputFire);

    SauCycleInputs granted;
    granted.outputRequest = true;
    granted.outputGrant = true;
    observation = model.tick(granted);
    EXPECT_EQ(observation.cycle, uint64_t{20});
    EXPECT_TRUE(observation.engineOutputFire);
    EXPECT_EQ(observation.outputCounter, uint64_t{0});

    SauCycleInputs registeredButBlocked;
    registeredButBlocked.outputGrant = false;
    observation = model.tick(registeredButBlocked);
    EXPECT_EQ(observation.cycle, uint64_t{21});
    EXPECT_TRUE(observation.rowScoreValid);
    EXPECT_EQ(observation.rowSequence, uint64_t{0});
    EXPECT_EQ(observation.outputSlots[0], uint16_t{0xffb9});
    EXPECT_EQ(observation.outputSlots[1], uint16_t{0x0058});
    EXPECT_EQ(observation.outputSlots[2], uint16_t{0x006f});
    EXPECT_FALSE(observation.engineOutputFire);

    observation = model.tick();
    EXPECT_EQ(observation.cycle, uint64_t{22});
    EXPECT_FALSE(observation.rowScoreValid);
    EXPECT_TRUE(observation.engineOutputFire);
    EXPECT_EQ(observation.outputCounter, uint64_t{1});

    observation = model.tick();
    EXPECT_EQ(observation.cycle, uint64_t{23});
    EXPECT_TRUE(observation.rowScoreValid);
    EXPECT_EQ(observation.rowSequence, uint64_t{1});
    EXPECT_EQ(observation.outputSlots[0], uint16_t{0x006d});
    EXPECT_EQ(observation.outputSlots[1], uint16_t{0xff80});
    EXPECT_EQ(observation.outputSlots[2], uint16_t{0xff80});
    EXPECT_TRUE(observation.calFinish);
}

TEST(SauCycleModel, FullArrayTouchesAll256PesAndOutputs16Rows)
{
    SauCycleModel model;
    model.tick(launchInputs(9, 16, 16));
    SauPeMask seenMac{};
    SauPeMask seenAdd{};
    uint64_t outputRows = 0;
    SauCycleObservation observation;
    for (uint64_t cycle = 1; cycle <= 47; ++cycle) {
        SauCycleInputs inputs;
        if (cycle <= 9) {
            inputs = streamInputs(1, 1);
        }
        if (cycle == 31) {
            inputs.outputRequest = true;
        }
        observation = model.tick(inputs);
        for (uint64_t word = 0; word < seenMac.size(); ++word) {
            seenMac[word] |= observation.macCommitMask[word];
            seenAdd[word] |= observation.addCommitMask[word];
        }
        if (observation.rowScoreValid) {
            ++outputRows;
            for (const auto slot : observation.outputSlots) {
                EXPECT_EQ(slot, uint16_t{9});
            }
        }
    }

    for (const auto word : seenMac) {
        EXPECT_EQ(word, std::numeric_limits<uint64_t>::max());
    }
    for (const auto word : seenAdd) {
        EXPECT_EQ(word, std::numeric_limits<uint64_t>::max());
    }
    EXPECT_TRUE(peMaskBit(seenMac, 0, 0));
    EXPECT_TRUE(peMaskBit(seenMac, 0, 1));
    EXPECT_TRUE(peMaskBit(seenMac, 0, 14));
    EXPECT_TRUE(peMaskBit(seenMac, 0, 15));
    EXPECT_TRUE(peMaskBit(seenMac, 1, 0));
    EXPECT_TRUE(peMaskBit(seenMac, 15, 15));
    EXPECT_EQ(outputRows, uint64_t{16});
    EXPECT_TRUE(observation.calFinish);
    EXPECT_EQ(observation.state, SauEngineState::Storage);

    observation = model.tick();
    EXPECT_FALSE(observation.calFinish);
    EXPECT_EQ(observation.state, SauEngineState::Idle);
}

TEST(SauCycleModel, K567SaturationCommitCyclesRemainProvisional)
{
    for (const auto weight : {int8_t{-128}, int8_t{127}}) {
        SCOPED_TRACE(static_cast<int>(weight));
        SauCycleModel model;
        model.tick(launchInputs(567, 1, 1));
        SauCycleObservation observation;
        for (uint64_t input = 1; input <= 567; ++input) {
            observation = model.tick(streamInputs(-128, weight));
            if (weight == -128 && observation.cycle == 515) {
                EXPECT_EQ(
                    observation.peStates[0].accumulator,
                    int32_t{8372224});
            }
            if (weight == -128 && observation.cycle == 516) {
                EXPECT_EQ(
                    observation.peStates[0].accumulator,
                    Accumulator24Max);
            }
            if (weight == 127 && observation.cycle == 520) {
                EXPECT_EQ(
                    observation.peStates[0].accumulator,
                    int32_t{-8388096});
            }
            if (weight == 127 && observation.cycle == 521) {
                EXPECT_EQ(
                    observation.peStates[0].accumulator,
                    Accumulator24Min);
            }
        }
    }
}

TEST(SauCycleModel, RejectsInvalidProtocolAndResetClearsQueues)
{
    SauCycleModel model;
    auto invalid = launchInputs(0, 1, 1);
    EXPECT_THROW(model.tick(invalid), std::invalid_argument);

    model.reset();
    model.tick(launchInputs(9, 1, 1));
    auto idleAfterLaunch = model.tick();
    EXPECT_EQ(idleAfterLaunch.state, SauEngineState::Idle);
    model.tick(streamInputs(1, 1));
    EXPECT_THROW(model.tick(), std::logic_error);
    model.reset();
    EXPECT_EQ(model.cycle(), uint64_t{0});
    EXPECT_EQ(model.state(), SauEngineState::Idle);

    auto launchAndInput = launchInputs(9, 1, 1);
    launchAndInput.inputValid = true;
    EXPECT_THROW(model.tick(launchAndInput), std::logic_error);
}

} // anonymous namespace
} // namespace gem5::sau_n
