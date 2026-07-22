#include "sau_n/streaming_pipeline_contract.hh"

#include <limits>
#include <stdexcept>

namespace gem5::sau_n
{
namespace
{

uint64_t
maskPopulation(uint16_t mask)
{
    uint64_t count = 0;
    while (mask != 0) {
        count += mask & 1U;
        mask >>= 1;
    }
    return count;
}

uint16_t
prefixMask(uint64_t count)
{
    if (count > SauRows) {
        throw std::out_of_range("prefix mask count exceeds SA rows");
    }
    return count == SauRows ? std::numeric_limits<uint16_t>::max() :
        static_cast<uint16_t>((uint32_t{1} << count) - 1);
}

void
validateRawCoordinate(
    const StreamingSpatialCoordinate &coordinate, uint64_t lane,
    bool expectedValid)
{
    if (coordinate.valid != expectedValid) {
        throw std::invalid_argument(
            "raw spatial mask and coordinate validity disagree");
    }
    if (!coordinate.valid &&
        (coordinate.n != 0 || coordinate.oh != 0 || coordinate.ow != 0)) {
        throw std::invalid_argument(
            "invalid raw coordinate must be canonical zero");
    }
    if (lane >= SauRows) {
        throw std::out_of_range("raw spatial lane exceeds SA rows");
    }
}

} // anonymous namespace

PipelineDerivedConfig
validateStreamingConfig(const PipelineResolvedConfig &config)
{
    const auto derived = validateAndDerive(config);
    if (config.im2col.strideH != config.im2col.strideW ||
        (config.im2col.strideH != 1 && config.im2col.strideH != 2)) {
        throw std::invalid_argument(
            "streaming stride_h/stride_w must be equal and in {1, 2}");
    }
    if (config.im2col.padTop != config.im2col.padLeft ||
        config.im2col.padTop > 1) {
        throw std::invalid_argument(
            "streaming pad_top/pad_left must be equal and in {0, 1}");
    }
    if (config.im2col.dilationH != 1 || config.im2col.dilationW != 1) {
        throw std::invalid_argument(
            "streaming dilation_h/dilation_w must both be 1");
    }
    return derived;
}

bool
isCanonicalPrefixMask(uint16_t mask)
{
    return mask == prefixMask(maskPopulation(mask));
}

CompactedSpatialPayload
compactSpatialPayload(const RawSpatialPayload &raw)
{
    if (raw.spatialMask == 0) {
        throw std::invalid_argument("raw spatial mask must be nonzero");
    }

    CompactedSpatialPayload compacted;
    for (uint64_t source = 0; source < SauRows; ++source) {
        const bool valid =
            (raw.spatialMask & (uint16_t{1} << source)) != 0;
        validateRawCoordinate(raw.coordinates[source], source, valid);
        if (!valid) {
            if (raw.activations[source] != 0) {
                throw std::invalid_argument(
                    "invalid raw activation lane must be canonical zero");
            }
            continue;
        }
        for (uint64_t earlier = 0; earlier < source; ++earlier) {
            if ((raw.spatialMask & (uint16_t{1} << earlier)) != 0 &&
                raw.coordinates[earlier] == raw.coordinates[source]) {
                throw std::invalid_argument(
                    "raw spatial coordinates must be unique");
            }
        }
        const uint64_t destination = compacted.validRows;
        compacted.activations[destination] = raw.activations[source];
        compacted.sourceLanes[destination] =
            static_cast<uint8_t>(source);
        compacted.coordinates[destination] = raw.coordinates[source];
        ++compacted.validRows;
    }
    compacted.spatialMask = prefixMask(compacted.validRows);
    return compacted;
}

void
validateVectorTag(const StreamingVectorTag &tag, uint64_t k)
{
    if (k == 0 || k > SauMaxChannels * 9 || k % 9 != 0) {
        throw std::invalid_argument(
            "streaming K must be a nonzero multiple of 9");
    }
    if (tag.ocGroup != 0) {
        throw std::invalid_argument("streaming oc_group must be zero");
    }
    if (tag.validColumns == 0 || tag.validColumns > SauColumns) {
        throw std::invalid_argument(
            "streaming valid_columns must be in [1, 16]");
    }
    if (tag.c >= k / 9 || tag.kh >= 3 || tag.kw >= 3 ||
        tag.kIndex >= k) {
        throw std::invalid_argument("streaming vector tag exceeds K bounds");
    }
    const uint64_t expectedK = tag.c * 9 + tag.kh * 3 + tag.kw;
    if (tag.kIndex != expectedK) {
        throw std::invalid_argument("streaming vector tag K order mismatch");
    }
    if (tag.tileFirst != (tag.kIndex == 0) ||
        tag.tileLast != (tag.kIndex + 1 == k)) {
        throw std::invalid_argument("streaming tile boundary tag mismatch");
    }
}

void
validateVectorSequence(
    const StreamingVectorTag &previousTag,
    const StreamingVectorTag &currentTag,
    uint64_t k)
{
    validateVectorTag(previousTag, k);
    validateVectorTag(currentTag, k);
    if (currentTag.tileIndex == previousTag.tileIndex) {
        if (previousTag.tileLast ||
            currentTag.kIndex != previousTag.kIndex + 1) {
            throw std::invalid_argument(
                "streaming K sequence is not contiguous within a tile");
        }
        return;
    }
    if (!previousTag.tileLast || !currentTag.tileFirst ||
        currentTag.tileIndex != previousTag.tileIndex + 1) {
        throw std::invalid_argument(
            "streaming tile sequence is not contiguous");
    }
}

void
validateSameTileMetadata(
    const StreamingVectorTag &previousTag,
    const CompactedSpatialPayload &previousPayload,
    const StreamingVectorTag &currentTag,
    const CompactedSpatialPayload &currentPayload)
{
    if (previousTag.tileIndex != currentTag.tileIndex ||
        previousTag.ocGroup != currentTag.ocGroup ||
        previousTag.validColumns != currentTag.validColumns ||
        previousPayload.spatialMask != currentPayload.spatialMask ||
        previousPayload.validRows != currentPayload.validRows ||
        previousPayload.sourceLanes != currentPayload.sourceLanes ||
        previousPayload.coordinates != currentPayload.coordinates) {
        throw std::invalid_argument(
            "streaming metadata changed within one tile");
    }
}

ElasticAdvanceDecision
decideElasticAdvance(const ElasticAdvanceInputs &inputs)
{
    if (inputs.fifoCount > StreamingFifoDepth) {
        throw std::out_of_range("streaming FIFO count exceeds depth");
    }
    if (inputs.fifoPop && inputs.fifoCount == 0) {
        throw std::logic_error("streaming FIFO cannot pop while empty");
    }

    ElasticAdvanceDecision decision;
    decision.fifoPushReady =
        inputs.fifoCount < StreamingFifoDepth || inputs.fifoPop;
    decision.s2Ready = !inputs.s2Valid || decision.fifoPushReady;
    decision.s1Ready = !inputs.s1Valid ||
        (inputs.s1CanRetire && decision.s2Ready);
    decision.s0Ready = !inputs.s0Valid || decision.s1Ready;
    decision.producerReady = decision.s0Ready && inputs.moreVectors;
    decision.s2ToFifo = inputs.s2Valid && decision.fifoPushReady;
    decision.s1ToS2 =
        inputs.s1Valid && inputs.s1CanRetire && decision.s2Ready;
    decision.s0ToS1 = inputs.s0Valid && decision.s1Ready;
    decision.producerToS0 = inputs.moreVectors && decision.s0Ready;
    return decision;
}

ElasticFifoDecision
decideElasticFifo(uint64_t count, bool pushValid, bool popRequest)
{
    if (count > StreamingFifoDepth) {
        throw std::out_of_range("streaming FIFO count exceeds depth");
    }
    if (popRequest && count == 0) {
        throw std::logic_error("streaming FIFO cannot pop while empty");
    }
    ElasticFifoDecision decision;
    decision.pushReady = count < StreamingFifoDepth || popRequest;
    decision.push = pushValid && decision.pushReady;
    decision.pop = popRequest;
    decision.nextCount = count + (decision.push ? 1 : 0) -
        (decision.pop ? 1 : 0);
    if (decision.nextCount > StreamingFifoDepth) {
        throw std::logic_error("streaming FIFO conservation failed");
    }
    return decision;
}

StreamingConsumerDecision
decideStreamingConsumer(
    StreamingConsumerState state,
    bool fifoValid,
    const StreamingVectorTag &headTag,
    uint64_t activeTile,
    uint64_t acceptedK,
    uint64_t k)
{
    if (acceptedK > k) {
        throw std::out_of_range("accepted K exceeds configured K");
    }
    StreamingConsumerDecision decision;
    switch (state) {
      case StreamingConsumerState::Idle:
        if (fifoValid) {
            validateVectorTag(headTag, k);
            if (!headTag.tileFirst || headTag.kIndex != 0) {
                throw std::logic_error(
                    "idle consumer requires a tile-first FIFO head");
            }
            decision.beginLaunch = true;
        }
        break;
      case StreamingConsumerState::Launch:
        if (!fifoValid || headTag.tileIndex != activeTile ||
            !headTag.tileFirst || headTag.kIndex != 0) {
            throw std::logic_error(
                "launch consumer must retain the tile-first FIFO head");
        }
        validateVectorTag(headTag, k);
        decision.launch = true;
        break;
      case StreamingConsumerState::AcceptK:
        decision.peReady = acceptedK < k;
        if (fifoValid) {
            validateVectorTag(headTag, k);
            if (headTag.tileIndex != activeTile ||
                headTag.kIndex != acceptedK) {
                throw std::logic_error(
                    "FIFO head does not match active tile/K");
            }
            decision.inputValid = true;
        }
        decision.inputFire = decision.inputValid && decision.peReady;
        break;
      case StreamingConsumerState::WaitResult:
      case StreamingConsumerState::DrainOutput:
        break;
    }
    return decision;
}

SauInputCycleDecision
decideSauInputCycle(
    SauInputProtocol protocol,
    bool streamActive,
    uint64_t acceptedInputs,
    uint64_t calcCycles,
    bool inputFire,
    bool previouslyScheduledMacDue)
{
    if (calcCycles == 0 || calcCycles > SauMaxChannels * 9 ||
        acceptedInputs > calcCycles) {
        throw std::invalid_argument("invalid SA input progress");
    }
    if (inputFire && acceptedInputs == calcCycles) {
        throw std::logic_error("SA input exceeds configured K");
    }
    if (protocol == SauInputProtocol::StrictRtlContinuous &&
        streamActive && acceptedInputs != 0 &&
        acceptedInputs < calcCycles && !inputFire) {
        throw std::logic_error(
            "strict SA input stream cannot contain bubbles");
    }
    return {
        inputFire,
        previouslyScheduledMacDue,
        acceptedInputs + (inputFire ? 1 : 0),
    };
}

void
validateDrainedConservation(const StreamingConservationCounts &counts)
{
    if (counts.expectedVectors == 0 || counts.expectedTiles == 0) {
        throw std::invalid_argument(
            "streaming expected vector/tile counts must be nonzero");
    }
    if (counts.producerAccepted != counts.expectedVectors ||
        counts.s2Pushed != counts.expectedVectors ||
        counts.fifoPushed != counts.expectedVectors ||
        counts.fifoPopped != counts.expectedVectors ||
        counts.peAccepted != counts.expectedVectors) {
        throw std::logic_error(
            "streaming vector conservation failed at drained");
    }
    if (counts.tilesGenerated != counts.expectedTiles ||
        counts.tilesLaunched != counts.expectedTiles ||
        counts.tilesCompleted != counts.expectedTiles) {
        throw std::logic_error(
            "streaming tile conservation failed at drained");
    }
}

} // namespace gem5::sau_n
