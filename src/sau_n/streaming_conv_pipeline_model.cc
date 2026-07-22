#include "sau_n/streaming_conv_pipeline_model.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "sau_n/sau_generators.hh"

namespace gem5::sau_n
{

StreamingConvPipelineModel::StreamingConvPipelineModel(
    const PipelineResolvedConfig &config,
    const OutputReadyConfig &ready)
    : resolved(config), dimensions(validateStreamingConfig(resolved)),
      readyConfig(ready), producer(resolved), sauModel(InputProtocol)
{
    validateOutputReady(readyConfig);
    if (dimensions.expectedOutputs >
        std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error(
            "streaming output count exceeds host container size");
    }
    collectedOutputs.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
    outputWritten.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
}

SauCycleInputs
StreamingConvPipelineModel::buildSauInputs(
    const StreamingConsumerDecision &decision,
    const StreamingFifoEntry &head) const
{
    SauCycleInputs inputs;
    inputs.outputGrant = outputReady(cycleNumber, readyConfig);
    if (decision.launch) {
        if (!activeEntry) {
            throw std::logic_error(
                "streaming launch requires active tile metadata");
        }
        inputs.insValid = true;
        inputs.config.calcCycles = dimensions.k;
        inputs.config.validRows = activeEntry->payload.validRows;
        inputs.config.validColumns = resolved.outChannels;
        inputs.config.cutbit = resolved.cutbit;
        for (uint64_t column = 0;
             column < resolved.outChannels; ++column) {
            inputs.config.biases[column] = biasValue(
                resolved.biasGenerator, column);
        }
    }
    if (decision.inputFire) {
        inputs.inputValid = true;
        for (uint64_t row = 0; row < SauRows; ++row) {
            inputs.activations[row] = signedInt8(
                head.payload.activations[row]);
        }
        for (uint64_t column = 0;
             column < resolved.outChannels; ++column) {
            inputs.weights[column] = weightValue(
                resolved.weightGenerator, column,
                head.tag.c, head.tag.kh, head.tag.kw);
        }
    }
    return inputs;
}

uint64_t
StreamingConvPipelineModel::outputIndex(
    const StreamingSpatialCoordinate &coordinate,
    uint64_t outputChannel) const
{
    if (!coordinate.valid || outputChannel >= resolved.outChannels) {
        throw std::out_of_range(
            "invalid streaming pipeline output coordinate");
    }
    uint64_t index = checkedMultiply(
        coordinate.n, resolved.outChannels,
        "streaming NCHW output index");
    index = checkedAdd(
        index, outputChannel, "streaming NCHW output index");
    index = checkedMultiply(
        index, resolved.im2col.outH,
        "streaming NCHW output index");
    index = checkedAdd(
        index, coordinate.oh, "streaming NCHW output index");
    index = checkedMultiply(
        index, resolved.im2col.outW,
        "streaming NCHW output index");
    return checkedAdd(
        index, coordinate.ow, "streaming NCHW output index");
}

void
StreamingConvPipelineModel::collectOutput(
    const SauCycleObservation &observation)
{
    if (!observation.rowScoreValid) {
        return;
    }
    if (!activeEntry ||
        observation.rowSequence != tileOutputRows ||
        observation.rowSequence >= activeEntry->payload.validRows) {
        throw std::logic_error(
            "streaming SA output row does not match active tile");
    }
    const auto &coordinate =
        activeEntry->payload.coordinates[observation.rowSequence];
    for (uint64_t column = 0;
         column < resolved.outChannels; ++column) {
        const uint16_t slot = observation.outputSlots[column];
        const int32_t signedSlot = slot < 0x8000 ?
            static_cast<int32_t>(slot) :
            static_cast<int32_t>(slot) - 65536;
        if (signedSlot < -128 || signedSlot > 127) {
            throw std::logic_error(
                "streaming SA output is not a signed INT8 value");
        }
        const int8_t value = static_cast<int8_t>(signedSlot);
        if (signExtendedInt8Slot(value) != slot) {
            throw std::logic_error(
                "streaming SA output is not canonical sign extension");
        }
        const uint64_t index = outputIndex(coordinate, column);
        if (index >= outputWritten.size()) {
            throw std::out_of_range(
                "streaming NCHW output index exceeds collector");
        }
        if (outputWritten[static_cast<std::size_t>(index)]) {
            throw std::logic_error("duplicate streaming NCHW output");
        }
        outputWritten[static_cast<std::size_t>(index)] = true;
        collectedOutputs[static_cast<std::size_t>(index)] = value;
        counters.outputElements = checkedAdd(
            counters.outputElements, 1,
            "streaming output element count");
    }
    tileOutputRows = checkedAdd(
        tileOutputRows, 1, "streaming tile output row count");
    counters.outputRows = checkedAdd(
        counters.outputRows, 1, "streaming output row count");
}

void
StreamingConvPipelineModel::completeActiveTile()
{
    if (!activeEntry ||
        tileOutputRows != activeEntry->payload.validRows ||
        acceptedK != dimensions.k) {
        throw std::logic_error(
            "streaming tile completed before all work retired");
    }
    counters.tilesCompleted = checkedAdd(
        counters.tilesCompleted, 1,
        "streaming completed tile count");
    activeEntry.reset();
    acceptedK = 0;
    tileOutputRows = 0;
}

void
StreamingConvPipelineModel::checkInvariants() const
{
    if (fifoCount > StreamingFifoDepth ||
        fifoReadPointer >= StreamingFifoDepth ||
        fifoWritePointer >= StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO state exceeds bounds");
    }
    const uint64_t pointerDistance =
        (fifoWritePointer + StreamingFifoDepth - fifoReadPointer) %
        StreamingFifoDepth;
    if (pointerDistance != fifoCount % StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO pointers/count diverged");
    }
    if (counters.fifoPushes < counters.fifoPops ||
        counters.fifoPushes - counters.fifoPops != fifoCount ||
        counters.fifoPushes != producer.stats().outputVectors ||
        counters.fifoPops != counters.peInputCycles) {
        throw std::logic_error("streaming FIFO/vector conservation failed");
    }
    if (acceptedK > dimensions.k ||
        counters.tilesCompleted > counters.tilesLaunched ||
        counters.tilesLaunched > dimensions.expectedTiles ||
        counters.outputElements > dimensions.expectedOutputs) {
        throw std::logic_error("streaming pipeline count exceeds bounds");
    }
    const uint64_t activeAccepted = activeEntry ? acceptedK : 0;
    const uint64_t expectedPops = checkedAdd(
        checkedMultiply(
            counters.tilesCompleted, dimensions.k,
            "streaming accepted input conservation"),
        activeAccepted, "streaming accepted input conservation");
    if (counters.fifoPops != expectedPops ||
        counters.peLaunches != counters.tilesLaunched) {
        throw std::logic_error(
            "streaming consumer input/tile conservation failed");
    }
    const bool active =
        consumerState != StreamingConsumerState::Idle;
    if (active != activeEntry.has_value()) {
        throw std::logic_error(
            "streaming consumer/active metadata state diverged");
    }
    if (activeEntry && activeEntry->tag.tileIndex != activeTile) {
        throw std::logic_error("streaming active tile tag diverged");
    }
    const bool instructionIssued =
        consumerState == StreamingConsumerState::AcceptK ||
        consumerState == StreamingConsumerState::WaitResult ||
        consumerState == StreamingConsumerState::DrainOutput;
    const uint64_t expectedLaunched = counters.tilesCompleted +
        static_cast<uint64_t>(instructionIssued);
    if (counters.tilesLaunched != expectedLaunched) {
        throw std::logic_error(
            "streaming consumer launch/completion state diverged");
    }
    if (sauModel.protocol() != InputProtocol) {
        throw std::logic_error("streaming SA protocol changed");
    }
}

StreamingConvPipelineCycle
StreamingConvPipelineModel::tick()
{
    if (drainedAt) {
        throw std::logic_error("cannot tick a drained streaming pipeline");
    }
    const StreamingConsumerState oldConsumerState = consumerState;
    const uint64_t oldAcceptedK = acceptedK;
    const uint64_t oldFifoCount = fifoCount;
    const bool fifoValid = oldFifoCount != 0;
    const StreamingFifoEntry head = fifoValid ?
        fifo[fifoReadPointer] : StreamingFifoEntry{};
    const auto consumerDecision = decideStreamingConsumer(
        oldConsumerState, fifoValid, head.tag,
        activeTile, oldAcceptedK, dimensions.k);
    const bool fifoPop = consumerDecision.inputFire;
    const bool fifoPushReady =
        oldFifoCount < StreamingFifoDepth || fifoPop;

    PipelinedIm2ColCycle producerCycle;
    if (!producer.hasDrained()) {
        producerCycle = producer.tick(fifoPushReady);
    } else {
        producerCycle.cycle = cycleNumber;
        producerCycle.s0Ready = true;
        producerCycle.s1Ready = true;
        producerCycle.s2Ready = fifoPushReady;
        producerCycle.producerExhausted = true;
        producerCycle.drained = true;
    }
    const bool fifoPush = producerCycle.s2Fire;
    const auto fifoDecision = decideElasticFifo(
        oldFifoCount, producerCycle.s2Valid, fifoPop);
    if (fifoDecision.pushReady != fifoPushReady ||
        fifoDecision.push != fifoPush ||
        fifoDecision.pop != fifoPop) {
        throw std::logic_error(
            "streaming FIFO decision diverged from producer/consumer");
    }

    StreamingConvPipelineCycle observation;
    observation.cycle = cycleNumber;
    observation.consumerState = oldConsumerState;
    observation.acceptedK = oldAcceptedK;
    observation.activeTile = activeTile;
    observation.fifoCount = oldFifoCount;
    observation.fifoReadPointer = fifoReadPointer;
    observation.fifoWritePointer = fifoWritePointer;
    observation.fifoHeadValid = fifoValid;
    observation.fifoHead = head;
    observation.fifoPushReady = fifoPushReady;
    observation.fifoPush = fifoPush;
    observation.fifoPop = fifoPop;
    observation.consumer = consumerDecision;
    observation.producer = producerCycle;

    if (consumerDecision.beginLaunch) {
        if (head.tag.tileIndex != counters.tilesLaunched) {
            throw std::logic_error(
                "streaming launch tile sequence is not contiguous");
        }
        activeEntry = head;
        activeTile = head.tag.tileIndex;
        acceptedK = 0;
        tileOutputRows = 0;
    }
    observation.sauInputs = buildSauInputs(consumerDecision, head);
    observation.sau = sauModel.tick(observation.sauInputs);
    observation.outputCollected = observation.sau.rowScoreValid;
    collectOutput(observation.sau);

    counters.fifoOccupancySamples = checkedAdd(
        counters.fifoOccupancySamples, 1,
        "streaming FIFO occupancy sample count");
    counters.fifoOccupancySum = checkedAdd(
        counters.fifoOccupancySum, oldFifoCount,
        "streaming FIFO occupancy sum");
    counters.fifoPeakOccupancy = std::max(
        counters.fifoPeakOccupancy, oldFifoCount);
    if (oldFifoCount == StreamingFifoDepth) {
        counters.fifoFullCycles = checkedAdd(
            counters.fifoFullCycles, 1,
            "streaming FIFO full cycle count");
    }
    if (oldConsumerState == StreamingConsumerState::AcceptK &&
        oldAcceptedK < dimensions.k && !fifoPop) {
        counters.peInputBubbleCycles = checkedAdd(
            counters.peInputBubbleCycles, 1,
            "streaming PE input bubble count");
    }
    if (oldConsumerState == StreamingConsumerState::WaitResult ||
        oldConsumerState == StreamingConsumerState::DrainOutput) {
        counters.peBusyNotAcceptingCycles = checkedAdd(
            counters.peBusyNotAcceptingCycles, 1,
            "streaming PE busy-not-accepting count");
    }

    if (fifoPop) {
        fifoReadPointer =
            (fifoReadPointer + 1) % StreamingFifoDepth;
        counters.fifoPops = checkedAdd(
            counters.fifoPops, 1, "streaming FIFO pop count");
        counters.peInputCycles = checkedAdd(
            counters.peInputCycles, 1,
            "streaming PE input cycle count");
    }
    if (fifoPush) {
        fifo[fifoWritePointer] = producerCycle.output;
        fifoWritePointer =
            (fifoWritePointer + 1) % StreamingFifoDepth;
        counters.fifoPushes = checkedAdd(
            counters.fifoPushes, 1, "streaming FIFO push count");
    }
    fifoCount = fifoDecision.nextCount;
    counters.tilesGenerated =
        producer.stats().inputVectors / dimensions.k;

    switch (oldConsumerState) {
      case StreamingConsumerState::Idle:
        if (consumerDecision.beginLaunch) {
            consumerState = StreamingConsumerState::Launch;
        }
        break;
      case StreamingConsumerState::Launch:
        if (!consumerDecision.launch) {
            throw std::logic_error(
                "streaming LAUNCH cycle did not issue instruction");
        }
        counters.peLaunches = checkedAdd(
            counters.peLaunches, 1, "streaming PE launch count");
        counters.tilesLaunched = checkedAdd(
            counters.tilesLaunched, 1,
            "streaming launched tile count");
        consumerState = StreamingConsumerState::AcceptK;
        break;
      case StreamingConsumerState::AcceptK:
        if (fifoPop) {
            if (!activeEntry) {
                throw std::logic_error(
                    "streaming input fire requires active metadata");
            }
            validateSameTileMetadata(
                activeEntry->tag, activeEntry->payload,
                head.tag, head.payload);
            acceptedK = checkedAdd(
                acceptedK, 1, "streaming accepted K count");
            if (acceptedK == dimensions.k) {
                consumerState = StreamingConsumerState::WaitResult;
            }
        }
        break;
      case StreamingConsumerState::WaitResult:
        if (observation.sau.storageReady) {
            consumerState = StreamingConsumerState::DrainOutput;
        }
        break;
      case StreamingConsumerState::DrainOutput:
        if (observation.sau.calFinish) {
            completeActiveTile();
            consumerState = StreamingConsumerState::Idle;
        }
        break;
    }

    checkInvariants();
    const bool drained = producer.hasDrained() && fifoCount == 0 &&
        consumerState == StreamingConsumerState::Idle &&
        sauModel.state() == SauEngineState::Idle;
    if (drained) {
        validateDrainedConservation({
            dimensions.im2col.expectedVectors,
            producer.stats().inputVectors,
            producer.stats().outputVectors,
            counters.fifoPushes,
            counters.fifoPops,
            counters.peInputCycles,
            dimensions.expectedTiles,
            counters.tilesGenerated,
            counters.tilesLaunched,
            counters.tilesCompleted,
        });
        if (counters.outputElements != dimensions.expectedOutputs) {
            throw std::logic_error(
                "streaming pipeline drained with incomplete outputs");
        }
        if (checkedMultiply(
                counters.outputRows, resolved.outChannels,
                "streaming drained output rows") !=
            dimensions.expectedOutputs) {
            throw std::logic_error(
                "streaming pipeline drained with incomplete output rows");
        }
        drainedAt = cycleNumber;
        observation.drained = true;
    }
    cycleNumber = checkedAdd(
        cycleNumber, 1, "streaming pipeline cycle");
    return observation;
}

} // namespace gem5::sau_n
