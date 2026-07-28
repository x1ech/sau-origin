#include "sau/payload_datapath.hh"

#include <stdexcept>

namespace gem5::sau
{

StrictPayloadDatapath::StrictPayloadDatapath(
    const SauResourceConfigs &configs,
    const std::optional<SystolicArray::AccumulatorMatrix> &retained)
    : configs(configs),
      writePath(configs.input.padding),
      arbiter(configs.transposeReuse),
      resultSerializer(configs.output),
      outputRegister(configs.output)
{
    if (retained) {
        array.restoreAccumulators(*retained);
    }
    array.setKeepMode(configs.array.keepMode);
    arbiter.clearOnStart();
    restartReadoutProgram();
}

void
StrictPayloadDatapath::beginCycle()
{
    events = {};
    pendingArrayInput.reset();
}

void
StrictPayloadDatapath::restartReadoutProgram()
{
    readout.emplace(configs.input.streamedCounters);
}

void
StrictPayloadDatapath::onMemoryDataVisible(uint64_t /* edge */,
                                           const MemoryBeat256 &payload,
                                           bool streamed)
{
    if (streamed) {
        ++streamedCount;
        streamQueue.push_back(payload);
        return;
    }
    ++residentCount;
    // The write pointer is the delay-aligned sequential store order;
    // the PAD_DELAY control alignment stays owned by the driver.
    writePath.write(file, writePointer, payload, false, writePointer == 0);
    ++writePointer;
}

void
StrictPayloadDatapath::onRegisterFileReadValid(uint64_t /* edge */)
{
    // The read program covers one full x/y/flow/instruction walk; the
    // reuse readout replays it back to back.
    if (readout->done()) {
        restartReadoutProgram();
    }
    readoutQueue.push_back(file.read(readout->pointer()));
    readout->advance();
}

void
StrictPayloadDatapath::onOperandAValid(uint64_t edge)
{
    if (readoutQueue.empty()) {
        ++underflows;
        return;
    }
    const MemoryBeat256 row = readoutQueue.front();
    readoutQueue.pop_front();
    ++operandACount;
    events.operandA = PayloadBoundaryTransfer{edge, row};
    if (!arbiter.loadsOperandA()) {
        return;
    }
    if (!arbiter.canAcceptRow()) {
        ++inputStalls;
        return;
    }
    const unsigned bank = arbiter.acceptRow(row);
    ++inputRows;
    events.transposerInput =
        TransposerBoundaryTransfer{{edge + 1, row}, bank};
    if (!firstRowAt) {
        firstRowAt = edge + 1;
    }
    // The output bank latches from the registered pre-edge loaded bank
    // when the first bank fills.
    if (!outputPhaseStarted && arbiter.columnReady()) {
        arbiter.startOutputPhase();
        outputPhaseStarted = true;
    }
}

void
StrictPayloadDatapath::onOperandBValid(uint64_t edge)
{
    if (streamQueue.empty()) {
        ++underflows;
        return;
    }
    const MemoryBeat256 row = streamQueue.front();
    streamQueue.pop_front();
    ++operandBCount;
    events.operandB = PayloadBoundaryTransfer{edge, row};
    if (!arbiter.loadsOperandB()) {
        return;
    }
    if (!arbiter.canAcceptRow()) {
        ++inputStalls;
        return;
    }
    const unsigned bank = arbiter.acceptRow(row);
    ++inputRows;
    events.transposerInput =
        TransposerBoundaryTransfer{{edge + 1, row}, bank};
    if (!firstRowAt) {
        firstRowAt = edge + 1;
    }
    if (!outputPhaseStarted && arbiter.columnReady()) {
        arbiter.startOutputPhase();
        outputPhaseStarted = true;
    }
}

void
StrictPayloadDatapath::onSaEnable(uint64_t edge)
{
    if (!arbiter.loadsOperandA() && !arbiter.loadsOperandB()) {
        return;
    }
    if (!outputPhaseStarted || !arbiter.columnReady()) {
        ++outputStalls;
        return;
    }
    const unsigned bank = arbiter.outputBank();
    const auto output = arbiter.readColumn();
    lastColumnData = output.data;
    events.transposerOutput =
        TransposerBoundaryTransfer{{edge, output.data}, bank};
    if (output.last && arbiter.columnReady()) {
        const unsigned prefetchBank = arbiter.outputBank();
        events.transposerPrefetch = TransposerBoundaryTransfer{
            {edge + 1, arbiter.peekColumn().data}, prefetchBank};
    }
    // The currently supported ATBD/Reuse-A path presents streamed B one
    // edge before SA_ENGINE samples it. Pair the registered B payload
    // with the transposed A column consumed above; do not invent a
    // fallback for the deferred reuse/transposition modes.
    if (configs.transposeReuse.transMode == SauTransMode::ATBD &&
        configs.transposeReuse.reuseA && arrayWeightPipeline) {
        const uint32_t inputsPerTile =
            SystolicArray::Rows * configs.controller.flowTimes;
        if (inputsPerTile == 0) {
            throw std::logic_error(
                "strict payload array tile has zero input extent");
        }
        const bool finish = arrayInputsInTile + 1 == inputsPerTile;
        pendingArrayInput = SystolicArrayInput{
            operandFromBeat(output.data),
            operandFromBeat(*arrayWeightPipeline),
            finish
        };
        events.arrayInput = ArrayInputBoundaryTransfer{
            edge,
            pendingArrayInput->activations,
            pendingArrayInput->weights,
            pendingArrayInput->finish
        };
        arrayInputsInTile = finish ? 0 : arrayInputsInTile + 1;
    }
    ++outputColumns;
    if (!firstColumnAt) {
        firstColumnAt = edge;
    }
}

void
StrictPayloadDatapath::requestArrayOutput()
{
    array.requestOutput(configs.array.cutbit);
    outputRequested = true;
}

void
StrictPayloadDatapath::onResultValid(uint64_t edge)
{
    if (!resultSerializer.outputReady()) {
        ++serializerOutputEmpty;
        return;
    }
    const SerializedResult result = resultSerializer.take();
    const OutputRegisterUpdate update = outputRegister.accept(result.data);
    ++outputUpdates;
    events.outputRegister = OutputRegisterBoundaryTransfer{edge, update};
}

void
StrictPayloadDatapath::onRegisterUnload(uint64_t edge,
                                        bool registerUnloadState)
{
    if (!registerUnloadState) {
        unloadRequestObserved = false;
        return;
    }
    if (!outputRegister.resultAccumDone()) {
        return;
    }
    if (!unloadRequestObserved) {
        // register_out_state samples REGISTER_UNLOAD && result_accum_done.
        // Its rising-edge flag becomes visible on the following tick.
        unloadRequestObserved = true;
        return;
    }
    if (!outputRegister.unloading() && !outputRegister.unloadDone()) {
        outputRegister.startUnload(configs.writeback.baseAddress);
        return;
    }

    const auto payload = outputRegister.tickUnload();
    if (!payload) {
        return;
    }
    writePayloads.push_back(*payload);
    ++unloadBeats;
    events.outputUnload = OutputUnloadBoundaryTransfer{edge, *payload};
}

const OutputRegisterUnload &
StrictPayloadDatapath::nextWritePayload() const
{
    if (writePayloads.empty()) {
        throw std::logic_error("strict payload write queue is empty");
    }
    return writePayloads.front();
}

OutputRegisterUnload
StrictPayloadDatapath::takeWritePayload()
{
    const OutputRegisterUnload payload = nextWritePayload();
    writePayloads.pop_front();
    return payload;
}

unsigned
StrictPayloadDatapath::bankOccupancy(const TransposerTinyBank &bank) const
{
    if (bank.outputReady()) {
        return TransposerTinyBank::Rows - bank.outputsTaken();
    }
    return bank.rowsAccepted();
}

void
StrictPayloadDatapath::sampleCycle(uint64_t edge)
{
    const unsigned occupancy =
        bankOccupancy(arbiter.bank(0)) + bankOccupancy(arbiter.bank(1));
    if (occupancy > 0) {
        ++busyCycles;
    }
    if (occupancy > maxOccupancy) {
        maxOccupancy = occupancy;
    }
    array.tick(pendingArrayInput);
    if (array.streamOutput()) {
        const OperandVector32x8 row = *array.streamOutput();
        events.arrayOutput =
            ArrayOutputBoundaryTransfer{edge, *array.streamRow(), row};
        if (!resultSerializer.canAccept()) {
            ++serializerInputBlocked;
        } else {
            resultSerializer.accept(row);
        }
        ++arrayRowsStreamed;
        if (arrayRowsStreamed == SystolicArray::Rows) {
            arrayRowsStreamed = 0;
            outputRequested = false;
        }
    }
    if (events.operandB) {
        arrayWeightPipeline = events.operandB->data;
    }
}

SystolicArray::AccumulatorMatrix
StrictPayloadDatapath::finishRetainedArrayState()
{
    while (!array.pipelineEmpty()) {
        array.tick();
    }
    return array.accumulators();
}

} // namespace gem5::sau
