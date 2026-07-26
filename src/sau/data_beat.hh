#ifndef __SAU_DATA_BEAT_HH__
#define __SAU_DATA_BEAT_HH__

#include <array>
#include <cstdint>
#include <stdexcept>

namespace gem5::sau
{

/**
 * PLAN3 Step 1 fixed-point data views and conversion boundaries.
 *
 * Byte-order contract, proven against the
 * tests/gem5/sau/functional_ref/int8_gemm_32x32x32_atbd_cutbit8 package:
 * byte k of a 256-bit external beat at address X is the byte at external
 * address X + k and occupies RTL rdata/wdata bit slice [8k+7:8k].  The
 * first captured sau_sram_rdata at 0x29120000 equals image lines {1,0}
 * concatenated, and the first output write beat at 0x29120c00 equals
 * final_output_memory.hex bytes 0..31 in ascending address order.
 *
 * Every lane vector uses the same mapping: lane k comes from beat byte k.
 */

/// External beat and lane geometry frozen by the RTL contract
/// (SA_CORE SRAM_DATA_WIDTH=256, SA_SIZE=32).
static constexpr unsigned BeatBytes = 32;
static constexpr unsigned BeatLanes = 32;

/// Signed 24-bit PE accumulator range (SA_PE.sv saturating accumulator).
static constexpr int32_t Int24Min = -(1 << 23);
static constexpr int32_t Int24Max = (1 << 23) - 1;

/// External 32-byte read payload.
struct MemoryBeat256
{
    std::array<uint8_t, BeatBytes> bytes{};

    bool
    operator==(const MemoryBeat256 &other) const
    {
        return bytes == other.bytes;
    }
};

/// 32-lane signed-int8 operand entering the input datapath.
struct OperandVector32x8
{
    std::array<int8_t, BeatLanes> lanes{};

    bool
    operator==(const OperandVector32x8 &other) const
    {
        return lanes == other.lanes;
    }
};

/// 32-lane signed 24-bit PE/snapshot accumulator state.  Each lane is
/// stored sign-extended in an int32_t and must stay within
/// [Int24Min, Int24Max].
struct AccumulatorVector32x24
{
    std::array<int32_t, BeatLanes> lanes{};

    bool
    operator==(const AccumulatorVector32x24 &other) const
    {
        return lanes == other.lanes;
    }
};

/// 32-lane signed 16-bit output register-file state
/// (register_file_out.sv lane accumulation).
struct OutputVector32x16
{
    std::array<int16_t, BeatLanes> lanes{};

    bool
    operator==(const OutputVector32x16 &other) const
    {
        return lanes == other.lanes;
    }
};

/// External 32-byte write payload after int8 saturation.
struct WriteBeat256
{
    std::array<uint8_t, BeatBytes> bytes{};

    bool
    operator==(const WriteBeat256 &other) const
    {
        return bytes == other.bytes;
    }
};

/// Sign-extend a raw 24-bit two's-complement value into an int32_t.
constexpr int32_t
signExtendInt24(uint32_t raw)
{
    const uint32_t masked = raw & 0xffffffu;
    if (masked & 0x800000u) {
        return static_cast<int32_t>(masked | 0xff000000u);
    }
    return static_cast<int32_t>(masked);
}

/// SA_PE.sv saturate_add_signed: the 24-bit accumulator clamps instead of
/// wrapping.  Both inputs must already be in-range 24-bit values.
constexpr int32_t
saturateAddInt24(int32_t accumulator, int32_t addend)
{
    // The sum of two in-range int24 values cannot overflow int32.
    const int32_t sum = accumulator + addend;
    if (sum > Int24Max) {
        return Int24Max;
    }
    if (sum < Int24Min) {
        return Int24Min;
    }
    return sum;
}

/**
 * SA_pkg::sat_truncate_func: arithmetic right shift of the signed 24-bit
 * MAC result by the raw 5-bit CSR cutbit, then saturation to signed int8.
 * cutbit is raw CSR data and accepts the full 0..31 domain.
 */
constexpr int8_t
satTruncateInt24(int32_t accumulator, unsigned cutbit)
{
    static_assert((-1 >> 1) == -1,
                  "sat_truncate_func requires arithmetic right shift");
    if (cutbit > 31) {
        throw std::invalid_argument(
            "cutbit exceeds the raw 5-bit CSR domain");
    }
    const int32_t shifted = accumulator >> cutbit;
    if (shifted > 127) {
        return 127;
    }
    if (shifted < -128) {
        return -128;
    }
    return static_cast<int8_t>(shifted);
}

/// register_file_out.sv lane accumulation: signed 16-bit two's-complement
/// wrap addition, not saturation.
constexpr int16_t
wrapAddInt16(int16_t accumulator, int16_t addend)
{
    return static_cast<int16_t>(static_cast<uint16_t>(
        static_cast<uint16_t>(accumulator) +
        static_cast<uint16_t>(addend)));
}

/// register_file_out.sv sat_signed8 writeout boundary: saturate one
/// signed 16-bit lane to signed int8.
constexpr int8_t
satSigned8(int16_t value)
{
    if (value > 127) {
        return 127;
    }
    if (value < -128) {
        return -128;
    }
    return static_cast<int8_t>(value);
}

/// Reinterpret beat byte k as signed int8 lane k.
inline OperandVector32x8
operandFromBeat(const MemoryBeat256 &beat)
{
    OperandVector32x8 operand;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        operand.lanes[lane] = static_cast<int8_t>(beat.bytes[lane]);
    }
    return operand;
}

/// Inverse of operandFromBeat; lane k becomes beat byte k.
inline MemoryBeat256
beatFromOperand(const OperandVector32x8 &operand)
{
    MemoryBeat256 beat;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        beat.bytes[lane] = static_cast<uint8_t>(operand.lanes[lane]);
    }
    return beat;
}

/// Writeout boundary: saturate each 16-bit output lane to int8 and place
/// it at the matching ascending-address byte of the external write beat.
inline WriteBeat256
writeBeatFromOutput(const OutputVector32x16 &output)
{
    WriteBeat256 beat;
    for (unsigned lane = 0; lane < BeatLanes; ++lane) {
        beat.bytes[lane] =
            static_cast<uint8_t>(satSigned8(output.lanes[lane]));
    }
    return beat;
}

} // namespace gem5::sau

#endif // __SAU_DATA_BEAT_HH__
