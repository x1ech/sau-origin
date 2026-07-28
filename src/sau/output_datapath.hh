#ifndef __SAU_OUTPUT_DATAPATH_HH__
#define __SAU_OUTPUT_DATAPATH_HH__

#include <array>
#include <cstdint>
#include <optional>

#include "sau/address_program.hh"
#include "sau/data_beat.hh"
#include "sau/resource_config.hh"

namespace gem5::sau
{

struct SerializedResult
{
    OutputVector32x16 data;
    bool last = false;
};

/**
 * PLAN3 Step 5 data-carrying copy of the result-side T2 transposer.
 *
 * One complete 32-row array snapshot occupies the finite bank before any
 * result can leave it. Normal and retain modes drain those rows in arrival
 * order. Flow-mode transpose drains columns with output lane k sourced from
 * input row 31-k, matching transposer_tiny's registered row reversal.
 *
 * The strict command driver continues to own the registered valid/last
 * timing. This resource owns only accepted payloads, capacity, ordering,
 * and the 32-result completion boundary.
 */
class ResultSerializer
{
  public:
    explicit ResultSerializer(const SauOutputResourceConfig &config);

    bool canAccept() const { return !ready; }
    bool outputReady() const { return ready; }
    unsigned rowsAccepted() const { return inputCount; }
    unsigned outputsTaken() const { return outputCount; }

    void accept(const OperandVector32x8 &row);
    SerializedResult peek() const;
    SerializedResult take();
    void reset();

  private:
    OutputVector32x16 output(unsigned index) const;

    const bool transposedOrder;
    std::array<OperandVector32x8, BeatLanes> rows{};
    unsigned inputCount = 0;
    unsigned outputCount = 0;
    bool ready = false;
};

struct OutputRegisterUpdate
{
    uint8_t address = 0;
    WriteBeat256 data;
    bool last = false;
};

struct OutputRegisterUnload
{
    Addr address = 0;
    uint8_t logicalAddress = 0;
    WriteBeat256 data;
    bool last = false;
};

/**
 * PLAN3 Step 5 register_file_out resource.
 *
 * The int8 GEMM path exposes 256 logical rows through two 128-entry SRAM
 * halves selected by address bit 7. Each accepted array result updates one
 * raw x/y/flow/instruction address. Normal/transpose flow modes ignore the
 * old SRAM value. Retain modes sign-extend its int8 lanes, perform the RTL's
 * 16-bit two's-complement wrap addition, and saturate each result back to
 * int8 before storage. Reading the updated entry for a consecutive same-
 * address acceptance is the architectural equivalent of the RTL RAW bypass.
 *
 * Once accumulation is complete, startUnload() launches the frozen RTL's
 * register_addr walk. tickUnload() models its registered address output plus
 * register_file_out's valid/address/data d1/d2 boundary: the first write beat
 * appears on the fourth tick after the launch pulse, then at one beat/tick.
 * Accumulation and unload cannot use the finite output resource concurrently.
 */
class OutputRegisterFile
{
  public:
    static constexpr unsigned Depth = 256;
    static constexpr unsigned HalfDepth = Depth / 2;

    explicit OutputRegisterFile(const SauOutputResourceConfig &config);

    OutputRegisterUpdate accept(const OutputVector32x16 &input);
    const WriteBeat256 &read(uint8_t address) const;

    uint8_t currentAddress() const { return address; }
    uint64_t acceptedResults() const { return acceptedCount; }
    bool resultAccumDone() const { return resultDone; }
    void clearCompletion();

    void startUnload(Addr baseAddress);
    std::optional<OutputRegisterUnload> tickUnload();
    bool unloading() const { return unloadActive; }
    bool unloadDone() const { return unloadDoneFlag; }

    void reset();

  private:
    bool endX() const;
    bool endY() const;
    bool endFlow() const;
    bool endInstruction() const;
    void advancePointer();
    WriteBeat256 compute(const OutputVector32x16 &input) const;
    WriteBeat256 &entry(uint8_t address);
    void clearUnloadControl();
    SauResidentAddressResourceConfig
    unloadAddressConfig(Addr baseAddress) const;

    const SauOutputResourceConfig config;
    std::array<WriteBeat256, HalfDepth> half0{};
    std::array<WriteBeat256, HalfDepth> half1{};
    uint8_t address = 0;
    uint8_t yBase = 0;
    uint8_t flowBase = 0;
    uint8_t instructionBase = 0;
    uint8_t countX = 0;
    uint8_t countY = 0;
    uint8_t countFlow = 0;
    uint8_t countInstruction = 0;
    uint64_t acceptedCount = 0;
    bool resultDone = false;

    std::optional<RtlResidentAddressProgram> unloadAddress;
    std::optional<OutputRegisterUnload> unloadAddressReg;
    std::optional<OutputRegisterUnload> unloadDelay1;
    std::optional<OutputRegisterUnload> unloadDelay2;
    unsigned unloadLaunchDelay = 0;
    bool unloadActive = false;
    bool unloadDoneFlag = false;
};

} // namespace gem5::sau

#endif // __SAU_OUTPUT_DATAPATH_HH__
