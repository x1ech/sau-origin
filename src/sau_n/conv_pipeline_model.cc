#include "sau_n/conv_pipeline_model.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

#include "sau_n/sau_generators.hh"

namespace gem5::sau_n
{
namespace
{

int64_t
arithmeticShiftRight(int32_t value, uint64_t shift)
{
    const int64_t divisor = int64_t{1} << shift;
    return value >= 0 ? value / divisor :
        -((-static_cast<int64_t>(value) + divisor - 1) / divisor);
}

bool
anyMaskBit(const SauPeMask &mask)
{
    for (const uint64_t word : mask) {
        if (word != 0) {
            return true;
        }
    }
    return false;
}

} // anonymous namespace

ConvPipelineModel::ConvPipelineModel(
    const PipelineResolvedConfig &config, const OutputReadyConfig &ready)
    : resolved(config), dimensions(validateAndDerive(resolved)),
      readyConfig(ready), im2colModel(resolved.im2col),
      tileBuffer(dimensions.k)
{
    validateOutputReady(readyConfig);
    validateSpatialTileMapping(resolved, dimensions);
    if (dimensions.expectedOutputs >
        std::numeric_limits<std::size_t>::max()) {
        throw std::overflow_error(
            "expected output count exceeds host container size");
    }
    collectedOutputs.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
    outputWritten.resize(
        static_cast<std::size_t>(dimensions.expectedOutputs));
}

SauCycleInputs
ConvPipelineModel::saInputsForState(PipelineState oldState) const
{
    SauCycleInputs inputs;
    inputs.outputGrant = outputReady(cycleNumber, readyConfig);
    if (!tileBuffer.empty()) {
        inputs.config.calcCycles = dimensions.k;
        inputs.config.validRows = tileBuffer.metadata().validRows;
        inputs.config.validColumns = resolved.outChannels;
        inputs.config.cutbit = resolved.cutbit;
        for (uint64_t column = 0; column < resolved.outChannels; ++column) {
            inputs.config.biases[column] = biasValue(
                resolved.biasGenerator, column);
        }
    }
    if (oldState == PipelineState::LaunchSa) {
        inputs.insValid = true;
    } else if (oldState == PipelineState::StreamK) {
        inputs.inputValid = true;
        const auto &feed = tileBuffer.at(streamK);
        for (uint64_t row = 0; row < SauRows; ++row) {
            inputs.activations[row] = signedInt8(feed.data[row]);
        }
        const uint64_t channel = streamK / 9;
        const uint64_t kernelIndex = streamK % 9;
        const uint64_t kh = kernelIndex / 3;
        const uint64_t kw = kernelIndex % 3;
        for (uint64_t column = 0; column < resolved.outChannels; ++column) {
            inputs.weights[column] = weightValue(
                resolved.weightGenerator, column, channel, kh, kw);
        }
    }
    return inputs;
}

void
ConvPipelineModel::acceptIm2ColFeed(const FeedVector &feed)
{
    if (tileBuffer.empty()) {
        tileBuffer.begin(spatialTileMetadata(
            resolved, dimensions, counters.tilesCollected));
        collectK = 0;
    }
    tileBuffer.push(collectK, feed);
    collectK = checkedAdd(collectK, 1, "tile collect K");
    counters.activationHandshakes = checkedAdd(
        counters.activationHandshakes, 1, "activation handshake count");
    if (tileBuffer.full()) {
        counters.tilesCollected = checkedAdd(
            counters.tilesCollected, 1, "collected tile count");
    }
}

uint64_t
ConvPipelineModel::outputIndex(
    const SpatialCoordinate &coordinate, uint64_t outputChannel) const
{
    if (!coordinate.valid || outputChannel >= resolved.outChannels) {
        throw std::out_of_range("invalid pipeline output coordinate");
    }
    uint64_t index = checkedMultiply(
        coordinate.n, resolved.outChannels, "NCHW output index");
    index = checkedAdd(index, outputChannel, "NCHW output index");
    index = checkedMultiply(index, resolved.im2col.outH, "NCHW output index");
    index = checkedAdd(index, coordinate.oh, "NCHW output index");
    index = checkedMultiply(index, resolved.im2col.outW, "NCHW output index");
    return checkedAdd(index, coordinate.ow, "NCHW output index");
}

void
ConvPipelineModel::collectOutput(const SauCycleObservation &observation)
{
    if (!observation.rowScoreValid) {
        return;
    }
    if (observation.rowSequence >= tileBuffer.metadata().validRows) {
        throw std::logic_error("SA output row exceeds active tile rows");
    }
    const auto &coordinate =
        tileBuffer.metadata().rows[observation.rowSequence];
    for (uint64_t column = 0; column < resolved.outChannels; ++column) {
        const uint16_t slot = observation.outputSlots[column];
        const int32_t signedSlot = slot < 0x8000 ?
            static_cast<int32_t>(slot) : static_cast<int32_t>(slot) - 65536;
        if (signedSlot < -128 || signedSlot > 127) {
            throw std::logic_error(
                "SA output slot is not a signed INT8 value");
        }
        const int8_t value = static_cast<int8_t>(signedSlot);
        if (signExtendedInt8Slot(value) != slot) {
            throw std::logic_error(
                "SA output slot is not canonical sign extension");
        }
        const uint64_t index = outputIndex(coordinate, column);
        if (index >= outputWritten.size()) {
            throw std::out_of_range("NCHW output index exceeds collector");
        }
        if (outputWritten[static_cast<std::size_t>(index)]) {
            throw std::logic_error("duplicate NCHW output write");
        }
        outputWritten[static_cast<std::size_t>(index)] = true;
        collectedOutputs[static_cast<std::size_t>(index)] = value;
        counters.outputElements = checkedAdd(
            counters.outputElements, 1, "output element count");
    }
    tileOutputRows = checkedAdd(tileOutputRows, 1, "tile output row count");
    counters.outputRows = checkedAdd(
        counters.outputRows, 1, "output row count");
    sauLastResultAt = cycleNumber;
}

void
ConvPipelineModel::completeTile()
{
    if (tileOutputRows != tileBuffer.metadata().validRows) {
        throw std::logic_error(
            "SA tile completed before all rows were collected");
    }
    counters.tilesCompleted = checkedAdd(
        counters.tilesCompleted, 1, "completed tile count");
    tileBuffer.clear();
    collectK = 0;
    streamK = 0;
    tileOutputRows = 0;
}

void
ConvPipelineModel::checkInvariants() const
{
    if (counters.tilesLaunched > counters.tilesCollected ||
        counters.tilesCompleted > counters.tilesLaunched ||
        counters.tilesCollected > dimensions.expectedTiles) {
        throw std::logic_error("pipeline tile conservation failed");
    }
    const uint64_t fullyReleasedTiles = counters.tilesCollected -
        (tileBuffer.full() ? 1 : 0);
    const uint64_t expectedAccepted = checkedAdd(
        checkedMultiply(
            fullyReleasedTiles, dimensions.k,
            "accepted activation conservation"),
        tileBuffer.empty() ? 0 : tileBuffer.count(),
        "accepted activation conservation");
    if (counters.activationHandshakes != expectedAccepted) {
        throw std::logic_error("tile buffer activation conservation failed");
    }
    if (counters.engineInputCycles > checkedMultiply(
            counters.tilesLaunched, dimensions.k,
            "maximum SA engine input cycles") ||
        counters.outputElements > dimensions.expectedOutputs) {
        throw std::logic_error("pipeline count exceeds derived expectation");
    }
}

ConvPipelineCycle
ConvPipelineModel::tick()
{
    if (drainedAt) {
        throw std::logic_error("cannot tick a drained pipeline");
    }
    const PipelineState oldState = pipelineState;
    ConvPipelineCycle observation;
    observation.cycle = cycleNumber;
    observation.state = oldState;
    observation.tileIndex = counters.tilesCompleted;
    observation.tileBufferCount = tileBuffer.count();
    observation.collectK = collectK;
    observation.streamK = streamK;
    observation.outputReady = outputReady(cycleNumber, readyConfig);
    counters.tileBufferOccupancySamples = checkedAdd(
        counters.tileBufferOccupancySamples, 1,
        "tile buffer occupancy sample count");
    counters.tileBufferOccupancySum = checkedAdd(
        counters.tileBufferOccupancySum, tileBuffer.count(),
        "tile buffer occupancy sum");
    counters.tileBufferPeakOccupancy = std::max(
        counters.tileBufferPeakOccupancy, tileBuffer.count());

    const bool feedReady = oldState == PipelineState::CollectTile &&
        !tileBuffer.full() &&
        counters.tilesCollected < dimensions.expectedTiles;
    observation.im2col = im2colModel.tick(feedReady);
    observation.im2colFeedHandshake = observation.im2col.fifoPop;
    if (observation.im2col.feedValid && !observation.im2col.feedReady) {
        counters.im2colBackpressureCycles = checkedAdd(
            counters.im2colBackpressureCycles, 1,
            "Im2Col backpressure cycle count");
    }
    if (observation.im2col.done && !im2colDoneAt) {
        im2colDoneAt = cycleNumber;
    }
    if (observation.im2colFeedHandshake) {
        acceptIm2ColFeed(observation.im2col.feed);
    }

    SauCycleInputs saInputs = saInputsForState(oldState);
    observation.saInputValid = saInputs.inputValid;
    observation.sauInputs = saInputs;
    observation.sau = sauModel.tick(saInputs);
    // The fused wrapper exposes the array's output-valid as output_request.
    // This is observational only; output acceptance is controlled by grant.
    observation.sauInputs.outputRequest = observation.sau.storageReady;
    if (saInputs.inputValid) {
        counters.engineInputCycles = checkedAdd(
            counters.engineInputCycles, 1, "SA engine input cycle count");
        counters.usefulMacs = checkedAdd(
            counters.usefulMacs,
            checkedMultiply(
                tileBuffer.metadata().validRows, resolved.outChannels,
                "useful MACs per engine input"),
            "useful MAC count");
    }
    if (anyMaskBit(observation.sau.macCommitMask)) {
        counters.arrayActiveCycles = checkedAdd(
            counters.arrayActiveCycles, 1, "array active cycle count");
    }
    for (uint64_t row = 0; row < SauRows; ++row) {
        if ((observation.sau.osValidMask & (uint16_t{1} << row)) == 0) {
            continue;
        }
        for (uint64_t column = 0; column < resolved.outChannels; ++column) {
            const int64_t shifted = arithmeticShiftRight(
                observation.sau.peStates[peIndex(row, column)].accumulator,
                resolved.cutbit);
            if (shifted > 127) {
                counters.positiveSaturations = checkedAdd(
                    counters.positiveSaturations, 1,
                    "positive output saturation count");
            } else if (shifted < -128) {
                counters.negativeSaturations = checkedAdd(
                    counters.negativeSaturations, 1,
                    "negative output saturation count");
            }
        }
    }
    if ((oldState == PipelineState::WaitResult ||
         oldState == PipelineState::DrainOutput) &&
        !observation.outputReady &&
        observation.sau.storageReady) {
        counters.outputBackpressureCycles = checkedAdd(
            counters.outputBackpressureCycles, 1,
            "output backpressure cycle count");
    }
    observation.outputCollected = observation.sau.rowScoreValid;
    collectOutput(observation.sau);
    observation.sauLastResult = observation.outputCollected &&
        counters.outputElements == dimensions.expectedOutputs;

    PipelineState nextState = oldState;
    switch (oldState) {
      case PipelineState::Idle:
        nextState = PipelineState::CollectTile;
        break;
      case PipelineState::CollectTile:
        if (tileBuffer.full()) {
            nextState = PipelineState::LaunchSa;
        }
        break;
      case PipelineState::LaunchSa:
        counters.tilesLaunched = checkedAdd(
            counters.tilesLaunched, 1, "launched tile count");
        streamK = 0;
        nextState = PipelineState::StreamK;
        break;
      case PipelineState::StreamK:
        streamK = checkedAdd(streamK, 1, "tile stream K");
        if (streamK == dimensions.k) {
            nextState = PipelineState::WaitResult;
        }
        break;
      case PipelineState::WaitResult:
        if (observation.sau.storageReady) {
            nextState = PipelineState::DrainOutput;
        }
        break;
      case PipelineState::DrainOutput:
        if (observation.sau.calFinish) {
            completeTile();
            nextState = counters.tilesCompleted == dimensions.expectedTiles ?
                PipelineState::Done : PipelineState::CollectTile;
        }
        break;
      case PipelineState::Done: {
        const PipelineDrainStatus status{
            !im2colModel.hasDrained(),
            im2colModel.hasDrained(),
            tileBuffer.empty(),
            observation.sau.state == SauEngineState::Idle,
            counters.tilesCompleted,
            counters.outputElements,
            false,
        };
        if (pipelineDrained(status, dimensions)) {
            if (counters.tilesCollected != dimensions.expectedTiles ||
                counters.tilesLaunched != dimensions.expectedTiles ||
                counters.activationHandshakes !=
                    checkedMultiply(
                        dimensions.expectedTiles, dimensions.k,
                        "expected activation handshakes") ||
                counters.engineInputCycles !=
                    checkedMultiply(
                        dimensions.expectedTiles, dimensions.k,
                        "expected SA engine input cycles") ||
                counters.usefulMacs != dimensions.expectedMacs) {
                throw std::logic_error("drained pipeline conservation failed");
            }
            drainedAt = cycleNumber;
            observation.drained = true;
        }
        break;
      }
    }

    pipelineState = nextState;
    checkInvariants();
    cycleNumber = checkedAdd(cycleNumber, 1, "pipeline model cycle");
    return observation;
}

} // namespace gem5::sau_n
