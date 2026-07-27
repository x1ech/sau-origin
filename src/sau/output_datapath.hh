#ifndef __SAU_OUTPUT_DATAPATH_HH__
#define __SAU_OUTPUT_DATAPATH_HH__

#include <array>
#include <cstdint>

#include "sau/data_beat.hh"
#include "sau/resource_config.hh"

namespace gem5::sau
{

struct OutputRegisterUpdate
{
    uint8_t address = 0;
    WriteBeat256 data;
    bool last = false;
};

/**
 * PLAN3 Step 5 accumulator-side register_file_out resource.
 *
 * The int8 GEMM path exposes 256 logical rows through two 128-entry SRAM
 * halves selected by address bit 7. Each accepted array result updates one
 * raw x/y/flow/instruction address. Normal/transpose flow modes ignore the
 * old SRAM value. Retain modes sign-extend its int8 lanes, perform the RTL's
 * 16-bit two's-complement wrap addition, and saturate each result back to
 * int8 before storage. Reading the updated entry for a consecutive same-
 * address acceptance is the architectural equivalent of the RTL RAW bypass.
 *
 * The registered SRAM port latency and unload address/data pipeline are
 * separate Step 5 resources; this class owns accepted update semantics,
 * finite storage, pointer state, and the sticky accumulation-done boundary.
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
    void clearCompletion() { resultDone = false; }
    void reset();

  private:
    bool endX() const;
    bool endY() const;
    bool endFlow() const;
    bool endInstruction() const;
    void advancePointer();
    WriteBeat256 compute(const OutputVector32x16 &input) const;
    WriteBeat256 &entry(uint8_t address);

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
};

} // namespace gem5::sau

#endif // __SAU_OUTPUT_DATAPATH_HH__
