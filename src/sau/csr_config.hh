#ifndef __SAU_CSR_CONFIG_HH__
#define __SAU_CSR_CONFIG_HH__

#include <cstdint>
#include <vector>

#include "sau/timing_policy.hh"
#include "sau/types.hh"

namespace gem5::sau
{

/** A write observed at the RTL SAU CSR interface. */
struct SauCsrWrite
{
    uint64_t cycle = 0;
    uint16_t address = 0;
    uint8_t operation = 0;
    uint64_t data = 0;
    bool accepted = false;
};

struct SauInputCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yBurst = 0;
    uint8_t flowStep = 0;
    uint8_t flowBurst = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionBurst = 0;
};

struct SauVerticalCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yCycle = 0;
    uint8_t flowStep = 0;
    uint8_t flowCycle = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionCycle = 0;
};

struct SauRegisterInputCsrConfig
{
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yCycle = 0;
    uint8_t cStep = 0;
    uint8_t cCycle = 0;
    uint8_t validYStart = 0;
    uint8_t validYEnd = 0;
    uint8_t validXStart = 0;
    uint8_t validXEnd = 0;
    uint8_t padding = 0;
};

struct SauOutputCsrConfig
{
    uint8_t xStep = 0;
    uint8_t xBurst = 0;
    uint8_t yStep = 0;
    uint8_t yBurst = 0;
    uint8_t flowStep = 0;
    uint8_t flowBurst = 0;
    uint8_t instructionStep = 0;
    uint8_t instructionBurst = 0;
    uint8_t registerXBurst = 0;
    uint8_t registerYStep = 0;
    uint8_t registerYCycle = 0;
    uint8_t registerCStep = 0;
    uint8_t registerCCycle = 0;
};

struct DecodedSauCommand
{
    SauCommand command;
    TimingPolicy timingPolicy;
};

/**
 * Register state with the field layout in hardware/src/sa_element/csr.sv.
 * It deliberately preserves raw control fields even when this model does not
 * yet support their corresponding execution modes.
 */
class SauCsrConfig
{
  public:
    static constexpr uint8_t DeviceId = 0x20;

    bool apply(const SauCsrWrite &write);
    DecodedSauCommand decode(uint64_t commandId) const;

    Addr verticalAddress = 0;
    Addr horizontalAddress = 0;
    Addr outputAddress = 0;
    Addr biasAddress = 0;
    uint8_t flowLoopTimes = 0;
    uint8_t convKernal = 0;
    uint8_t reuseMode = 0;
    uint8_t transMode = 0;
    uint8_t peWorkMode = 0;
    uint8_t saFlowMode = 0;
    uint8_t registerMode = 0;
    bool strideFlag = false;
    bool shiftFlag = false;
    uint8_t cutbit = 0;
    SauInputCsrConfig input;
    SauVerticalCsrConfig vertical;
    SauRegisterInputCsrConfig registerInput;
    SauOutputCsrConfig output;

  private:
    bool startActive = false;
    bool hasAppliedWrite = false;
    uint64_t lastAppliedCycle = 0;
    uint64_t startCycle = 0;
};

struct ReplayedSauCommand
{
    uint64_t startCycle = 0;
    DecodedSauCommand decoded;
};

std::vector<ReplayedSauCommand>
replayCsrWrites(const std::vector<SauCsrWrite> &writes);

} // namespace gem5::sau

#endif // __SAU_CSR_CONFIG_HH__
