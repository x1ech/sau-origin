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
    // A transposer bank that becomes full at an edge exposes ready/outCol
    // only after that edge.  Consumers on the fill edge still observe the
    // prior not-ready state.
    if (outputPhaseStartPending) {
        arbiter.startOutputPhase();
        outputPhaseStarted = true;
        outputPhaseStartPending = false;
    }
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
    if (configs.transposeReuse.transMode == SauTransMode::ABTD) {
        abtdResidentTail = payload;
    }
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
    MemoryBeat256 row;
    if (configs.transposeReuse.transMode == SauTransMode::ABTD &&
        !abtdResidentTailConsumed) {
        if (!abtdResidentTail) {
            ++underflows;
            return;
        }
        row = *abtdResidentTail;
        abtdResidentTailConsumed = true;
    } else {
        if (streamQueue.empty()) {
            ++underflows;
            return;
        }
        row = streamQueue.front();
        streamQueue.pop_front();
    }
    ++operandBCount;
    events.operandB = PayloadBoundaryTransfer{edge, row};
    if (!arbiter.loadsOperandB()) {
        return;
    }
    // ABTD bank loading is not a direct B-payload connection. Frozen
    // sa_feeder RTL registers B-valid as trans_load_valid, but the payload
    // itself comes through the input-switch mux one edge earlier.
    if (configs.transposeReuse.transMode == SauTransMode::ABTD) {
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
StrictPayloadDatapath::advanceFeederMux(uint64_t edge,
                                        uint8_t outputInputSwitch)
{
    if (configs.transposeReuse.transMode != SauTransMode::ABTD ||
        !configs.transposeReuse.reuseA) {
        return;
    }

    // trans_load_valid and data_i are both registered in sa_feeder. A
    // B-valid edge therefore loads the row selected on the preceding edge.
    if (pendingMuxTransposerRow) {
        const bool singleTileComplete =
            outputPhaseStarted && configs.controller.flowTimes == 1 &&
            configs.controller.instructionTimes == 1;
        if (singleTileComplete || !arbiter.canAcceptRow()) {
            ++inputStalls;
        } else {
            const unsigned bank = arbiter.acceptRow(*pendingMuxTransposerRow);
            ++inputRows;
            events.transposerInput = TransposerBoundaryTransfer{
                {edge, *pendingMuxTransposerRow}, bank};
            if (!firstRowAt) {
                firstRowAt = edge;
            }
            if (!outputPhaseStarted && arbiter.columnReady()) {
                outputPhaseStartPending = true;
            }
        }
        pendingMuxTransposerRow.reset();
    }

    if (!events.operandB) {
        return;
    }
    // sa_feeder.sv: data_i = input_switch[1] ? data_B_i : data_A_i.
    // An unselected/missing operand is the zero value on that wire.
    if ((outputInputSwitch & 0x2) != 0) {
        pendingMuxTransposerRow = events.operandB->data;
    } else if (events.operandA) {
        pendingMuxTransposerRow = events.operandA->data;
    } else {
        pendingMuxTransposerRow = MemoryBeat256{};
    }
}

void
StrictPayloadDatapath::onSaEnable(uint64_t edge, uint8_t outputInputSwitch)
{
    if (!arbiter.loadsOperandA() && !arbiter.loadsOperandB()) {
        return;
    }
    // Frozen sa_feeder.sv selects the array ports from raw input_switch:
    // 01 sends trans_sa_data left and registered B above. Before either
    // transposer output-select asserts, trans_sa_data is explicitly zero;
    // sa_en_i remains EN_i_d and therefore does not stall for bank ready.
    const bool transposerLeft =
        (outputInputSwitch & 0x3) == 0x1 &&
        (configs.transposeReuse.transMode == SauTransMode::ATBD ||
         configs.transposeReuse.transMode == SauTransMode::ABTD);
    const auto stageArrayInput =
        [this, edge, transposerLeft](const MemoryBeat256 &activation) {
            if (!transposerLeft || !configs.transposeReuse.reuseA ||
                !arrayWeightPipeline) {
                return;
            }
            const uint32_t inputsPerTile =
                SystolicArray::Rows * configs.controller.flowTimes;
            if (inputsPerTile == 0) {
                throw std::logic_error(
                    "strict payload array tile has zero input extent");
            }
            const bool finish = arrayInputsInTile + 1 == inputsPerTile;
            pendingArrayInput = SystolicArrayInput{
                operandFromBeat(activation),
                operandFromBeat(*arrayWeightPipeline), finish};
            events.arrayInput = ArrayInputBoundaryTransfer{
                edge, pendingArrayInput->activations,
                pendingArrayInput->weights, pendingArrayInput->finish};
            arrayInputsInTile = finish ? 0 : arrayInputsInTile + 1;
        };

    if (!outputPhaseStarted || !arbiter.columnReady()) {
        if (configs.transposeReuse.transMode == SauTransMode::ABTD &&
            transposerLeft) {
            stageArrayInput(MemoryBeat256{});
            return;
        }
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
    stageArrayInput(output.data);
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
