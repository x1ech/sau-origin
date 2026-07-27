#include "sau/payload_datapath.hh"

namespace gem5::sau
{

StrictPayloadDatapath::StrictPayloadDatapath(
    const SauResourceConfigs &configs)
    : configs(configs),
      writePath(configs.input.padding),
      arbiter(configs.transposeReuse)
{
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
        pendingArrayInput = SystolicArrayInput{
            operandFromBeat(output.data),
            operandFromBeat(*arrayWeightPipeline),
            output.last
        };
        events.arrayInput = ArrayInputBoundaryTransfer{
            edge,
            pendingArrayInput->activations,
            pendingArrayInput->weights,
            pendingArrayInput->finish
        };
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
        events.arrayOutput = ArrayOutputBoundaryTransfer{
            edge, *array.streamRow(), *array.streamOutput()
        };
    }
    if (events.operandB) {
        arrayWeightPipeline = events.operandB->data;
    }
}

} // namespace gem5::sau
