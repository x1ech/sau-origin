#include "sau/transposer.hh"

#include <cassert>

namespace gem5::sau
{

TransposerTinyBank::TransposerTinyBank(bool transposeEnabled,
                                       bool reuseEnabled)
    : transposeEnabled(transposeEnabled), reuseEnabled(reuseEnabled)
{
}

bool
TransposerTinyBank::inputReady() const
{
    return dataInReady;
}

bool
TransposerTinyBank::outputReady() const
{
    return outputReadyFlag;
}

bool
TransposerTinyBank::error() const
{
    return errorFlag;
}

unsigned
TransposerTinyBank::rowsAccepted() const
{
    return cntIn;
}

unsigned
TransposerTinyBank::outputsTaken() const
{
    return cntOut;
}

void
TransposerTinyBank::writeRow(const MemoryBeat256 &row)
{
    // The source stores and advances even when data_in_ready is low; it
    // only latches the error flag for that acceptance.
    if (!dataInReady) {
        errorFlag = true;
    }
    storage[cntIn] = row;
    if (cntIn == Rows - 1) {
        cntIn = 0;
        dataInReady = false;
        outputReadyFlag = true;
    } else {
        ++cntIn;
    }
}

MemoryBeat256
TransposerTinyBank::column(unsigned index) const
{
    // outCol lane a takes row (31-a), byte (index): the transposed
    // column with the source's row reversal, proven against the ABTD
    // golden's first prefetched trans0_outCol value.
    MemoryBeat256 result;
    for (unsigned lane = 0; lane < Rows; ++lane) {
        result.bytes[lane] = storage[Rows - 1 - lane].bytes[index];
    }
    return result;
}

TransposerTinyBank::Output
TransposerTinyBank::peekOutput() const
{
    assert(outputReadyFlag);
    Output output;
    output.data = transposeEnabled ? column(cntOut) : storage[cntOut];
    output.last = cntOut == Rows - 1;
    return output;
}

TransposerTinyBank::Output
TransposerTinyBank::readOutput()
{
    assert(outputReadyFlag);
    const Output output = peekOutput();
    if (output.last) {
        cntOut = 0;
        dataInReady = true;
        errorFlag = false;
        if (!reuseEnabled) {
            outputReadyFlag = false;
        }
    } else {
        ++cntOut;
    }
    return output;
}

void
TransposerTinyBank::clear()
{
    storage.fill(MemoryBeat256{});
    cntIn = 0;
    cntOut = 0;
    dataInReady = true;
    outputReadyFlag = false;
    errorFlag = false;
}

SaOperandRouting
saOperandRouting(SauTransMode mode, uint8_t inputSwitch)
{
    const bool transposerWork =
        mode == SauTransMode::ATBD || mode == SauTransMode::ABTD;
    SaOperandRouting routing;
    if (inputSwitch == 0x1) {
        routing.leftFromTransposer = transposerWork;
    } else if (inputSwitch == 0x2) {
        routing.aboveFromTransposer = transposerWork;
    }
    return routing;
}

TransposerArbiter::TransposerArbiter(
    const SauTransposeReuseResourceConfig &config)
    : config(config),
      // ABD without the (out-of-stage) conv path is the pure-output
      // mode: T0/T1 never take operand input.
      pureOutput(config.transMode == SauTransMode::ABD),
      // T0/T1 elaborate with transpose_en = (trans_mode != ABD) and
      // reuse_en tied low; sa_flow retention works by skipping the
      // command-start clear, not through the bank reuse input.
      bank0(config.transMode != SauTransMode::ABD, false),
      bank1(config.transMode != SauTransMode::ABD, false)
{
}

bool
TransposerArbiter::loadsOperandA() const
{
    return config.loadOperandA;
}

bool
TransposerArbiter::loadsOperandB() const
{
    return config.loadOperandB;
}

void
TransposerArbiter::clearOnStart()
{
    if (config.retainBanks) {
        return;
    }
    bank0.clear();
    bank1.clear();
    loadedBank = false;
    outputBankSel = false;
}

bool
TransposerArbiter::canAcceptRow() const
{
    return !pureOutput && (bank0.inputReady() || bank1.inputReady());
}

unsigned
TransposerArbiter::acceptRow(const MemoryBeat256 &row)
{
    assert(canAcceptRow());
    // Source steering policy: T0 first, T1 only when T0 cannot take
    // input.
    if (bank0.inputReady()) {
        bank0.writeRow(row);
        loadedBank = false;
        return 0;
    }
    bank1.writeRow(row);
    loadedBank = true;
    return 1;
}

bool
TransposerArbiter::columnReady() const
{
    return bank0.outputReady() || bank1.outputReady();
}

void
TransposerArbiter::startOutputPhase()
{
    // trans_output_phase_start latches the last loaded bank as the
    // output bank.
    outputBankSel = loadedBank;
}

TransposerTinyBank &
TransposerArbiter::selectedOutputBank()
{
    // trans0/1_output_sel: an exclusively ready bank wins; when both
    // are ready the registered output-bank selection decides.
    if (bank0.outputReady() && (!bank1.outputReady() || !outputBankSel)) {
        return bank0;
    }
    return bank1;
}

void
TransposerArbiter::followExclusiveReady()
{
    if (bank0.outputReady() && !bank1.outputReady()) {
        outputBankSel = false;
    } else if (bank1.outputReady() && !bank0.outputReady()) {
        outputBankSel = true;
    }
}

TransposerTinyBank::Output
TransposerArbiter::peekColumn() const
{
    assert(columnReady());
    return bank(outputBank()).peekOutput();
}

TransposerTinyBank::Output
TransposerArbiter::readColumn()
{
    assert(columnReady());
    auto &bank = selectedOutputBank();
    const auto output = bank.readOutput();
    followExclusiveReady();
    return output;
}

unsigned
TransposerArbiter::outputBank() const
{
    if (bank0.outputReady() && (!bank1.outputReady() || !outputBankSel)) {
        return 0;
    }
    return 1;
}

const TransposerTinyBank &
TransposerArbiter::bank(unsigned index) const
{
    return index == 0 ? bank0 : bank1;
}

} // namespace gem5::sau
