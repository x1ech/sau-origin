#include "sau/systolic_array.hh"

#include <stdexcept>

namespace gem5::sau
{

void
SystolicArray::tick(const std::optional<SystolicArrayInput> &input)
{
    const uint8_t priorSnapshotReady = snapshotReadyMask;
    const bool priorCalFinish = calFinishPulse;
    peFinishMask = 0;
    snapshotReadyMask = 0;
    calFinishPulse = false;
    streamOutputValue.reset();
    streamRowValue.reset();

    if (priorCalFinish) {
        storageReadyValue = false;
    } else if (priorSnapshotReady & 0x1) {
        storageReadyValue = true;
    }
    advanceResultStream(priorSnapshotReady);

    auto &ready = pipeline[currentCycle % PipelineSlots];
    for (const auto &event : ready) {
        commit(event);
    }
    pendingEvents -= ready.size();
    ready.clear();

    if (input) {
        if (finishSeen) {
            throw std::logic_error(
                "systolic-array input follows the finish token");
        }
        finishSeen = input->finish;
        ++acceptedInputCount;
        for (unsigned macroRow = 0; macroRow < MacroRows; ++macroRow) {
            for (unsigned macroColumn = 0;
                 macroColumn < MacroColumns; ++macroColumn) {
                MacroEvent event;
                event.macroRow = macroRow;
                event.macroColumn = macroColumn;
                for (unsigned lane = 0; lane < MacroSize; ++lane) {
                    const unsigned physicalRow =
                        macroRow * MacroSize + lane;
                    const unsigned column =
                        macroColumn * MacroSize + lane;
                    event.activations[lane] =
                        input->activations.lanes[Rows - 1 - physicalRow];
                    event.weights[lane] = input->weights.lanes[column];
                }
                event.finish = input->finish;
                const unsigned delay =
                    CalcDelay + macroRow + macroColumn;
                pipeline[(currentCycle + delay) % PipelineSlots]
                    .push_back(event);
                ++pendingEvents;
            }
        }
    }
    ++currentCycle;
}

void
SystolicArray::requestOutput(unsigned cutbit)
{
    if (cutbit > 31) {
        throw std::invalid_argument(
            "array stream cutbit exceeds the raw 5-bit CSR domain");
    }
    if (outputStartActive || resultStreamStarted) {
        throw std::logic_error(
            "systolic-array result stream already requested");
    }
    outputStartActive = true;
    streamCutbit = cutbit;
}

void
SystolicArray::commit(const MacroEvent &event)
{
    for (unsigned localRow = 0; localRow < MacroSize; ++localRow) {
        const unsigned row = event.macroRow * MacroSize + localRow;
        for (unsigned localColumn = 0;
             localColumn < MacroSize; ++localColumn) {
            const unsigned column =
                event.macroColumn * MacroSize + localColumn;
            pes[row][column].tick(SystolicPeInputs{
                event.activations[localRow],
                event.weights[localColumn],
                true,
                true,
                false
            });
            ++committedMacCount;
        }
    }
    if (!event.finish) {
        return;
    }

    peFinishMask |= event.macroColumn == 0
        ? uint8_t{1} << event.macroRow : 0;
    if (event.macroColumn == MacroColumns - 1) {
        snapshotReadyMask |= uint8_t{1} << event.macroRow;
    }
    for (unsigned localRow = 0; localRow < MacroSize; ++localRow) {
        const unsigned row = event.macroRow * MacroSize + localRow;
        for (unsigned localColumn = 0;
             localColumn < MacroSize; ++localColumn) {
            const unsigned column =
                event.macroColumn * MacroSize + localColumn;
            snapshot[row][column] = pes[row][column].accumulator();
        }
    }
}

void
SystolicArray::advanceResultStream(uint8_t priorSnapshotReady)
{
    std::array<bool, MacroRows> tokens{};
    tokens[0] = outputStartActive;
    for (unsigned macroRow = 1; macroRow < MacroRows; ++macroRow) {
        tokens[macroRow] =
            streamStates[macroRow - 1] == StreamState::Streaming &&
            streamCounters[macroRow - 1] == MacroSize - 1;
    }

    for (unsigned macroRow = 0; macroRow < MacroRows; ++macroRow) {
        const bool snapshotReady =
            priorSnapshotReady & (uint8_t{1} << macroRow);
        switch (streamStates[macroRow]) {
          case StreamState::Idle:
            streamCounters[macroRow] = 0;
            if (snapshotReady) {
                streamStates[macroRow] = tokens[macroRow]
                    ? StreamState::Streaming : StreamState::WaitToken;
            }
            break;
          case StreamState::WaitToken:
            if (tokens[macroRow]) {
                streamStates[macroRow] = StreamState::Streaming;
            }
            break;
          case StreamState::Streaming:
            if (streamCounters[macroRow] == MacroSize - 1) {
                streamStates[macroRow] = StreamState::Idle;
                streamCounters[macroRow] = 0;
            } else {
                ++streamCounters[macroRow];
            }
            break;
        }
    }

    if (streamStates[0] == StreamState::Streaming &&
        outputStartActive) {
        outputStartActive = false;
        resultStreamStarted = true;
    }
    for (unsigned macroRow = 0; macroRow < MacroRows; ++macroRow) {
        if (streamStates[macroRow] != StreamState::Streaming) {
            continue;
        }
        if (streamOutputValue) {
            throw std::logic_error(
                "overlapping systolic-array macro-row streams");
        }
        const unsigned row =
            macroRow * MacroSize + streamCounters[macroRow];
        OperandVector32x8 output;
        for (unsigned column = 0; column < Columns; ++column) {
            output.lanes[column] =
                satTruncateInt24(snapshot[row][column], streamCutbit);
        }
        streamOutputValue = output;
        streamRowValue = row;
        calFinishPulse = row == Rows - 1;
    }
}

void
SystolicArray::reset()
{
    for (auto &row : pes) {
        for (auto &pe : row) {
            pe.reset();
        }
    }
    for (auto &slot : pipeline) {
        slot.clear();
    }
    snapshot = {};
    streamStates = {};
    streamCounters = {};
    currentCycle = 0;
    acceptedInputCount = 0;
    committedMacCount = 0;
    pendingEvents = 0;
    peFinishMask = 0;
    snapshotReadyMask = 0;
    storageReadyValue = false;
    calFinishPulse = false;
    outputStartActive = false;
    resultStreamStarted = false;
    finishSeen = false;
    streamCutbit = 0;
    streamOutputValue.reset();
    streamRowValue.reset();
}

int32_t
SystolicArray::accumulator(unsigned row, unsigned column) const
{
    if (row >= Rows || column >= Columns) {
        throw std::out_of_range("systolic-array accumulator index");
    }
    return pes[row][column].accumulator();
}

SystolicArray::AccumulatorMatrix
SystolicArray::accumulators() const
{
    AccumulatorMatrix result;
    for (unsigned row = 0; row < Rows; ++row) {
        for (unsigned column = 0; column < Columns; ++column) {
            result[row][column] = pes[row][column].accumulator();
        }
    }
    return result;
}

OperandVector32x8
SystolicArray::quantizedRow(unsigned row, unsigned cutbit) const
{
    if (row >= Rows) {
        throw std::out_of_range("systolic-array quantized row index");
    }
    OperandVector32x8 result;
    for (unsigned column = 0; column < Columns; ++column) {
        result.lanes[column] = pes[row][column].quantized(cutbit);
    }
    return result;
}

} // namespace gem5::sau
