#ifndef __SAU_INPUT_DATAPATH_HH__
#define __SAU_INPUT_DATAPATH_HH__

#include <array>
#include <bitset>
#include <cstdint>

#include "sau/data_beat.hh"
#include "sau/types.hh"

namespace gem5::sau
{

/**
 * PLAN3 Step 3 input-side payload resources, copied from the frozen RTL
 * sources register_file_in.sv and padding_shifter.sv (hashes in
 * RTL_TIMING_PROVENANCE.md, verified against the local authoritative
 * checkout).  These model payload storage and pointer sequences; the
 * surrounding control-delay chains stay owned by the strict per-tick
 * skeletons.
 */

/**
 * stream_padding_shifter: a one-stage byte barrel shifter.  Each
 * accepted beat is shifted up by the raw 4-bit padding byte count; the
 * vacated low bytes come from zero at start-of-packet or from the tail
 * of the previously accepted beat.
 */
class StreamPaddingShifter
{
  public:
    explicit StreamPaddingShifter(uint8_t paddingBytes);

    /** Consume one accepted input beat and return its shifted output. */
    MemoryBeat256 shift(const MemoryBeat256 &data, bool startOfPacket);

  private:
    const uint8_t paddingBytes;
    MemoryBeat256 previous;
};

/**
 * register_file_in payload storage: REGDEPTH=256 entries of one 32-byte
 * beat plus the per-entry avail label.  A clear resets only the labels;
 * the reset storage contents are not a functional initializer (Step 0
 * state-lifetime table), and the RTL read port returns stored data
 * without label gating.
 */
class InputRegisterFile
{
  public:
    static constexpr unsigned Depth = 256;

    void write(uint8_t pointer, const MemoryBeat256 &data);
    void clearLabels();
    bool available(uint8_t pointer) const;
    const MemoryBeat256 &read(uint8_t pointer) const;

  private:
    std::array<MemoryBeat256, Depth> storage{};
    std::bitset<Depth> availLabel;
};

/**
 * feeder.sv operand-B payload chain for the int8 GEMM stage.  The
 * source's constant conv_reuse_flag=1 routes operand A exclusively
 * through the register-file readout and shift_register path, so this
 * chain carries only the streamed mem_ctrl data:
 *
 *   data_i -> data_i_d -> data_i_d2 -> data_i_case0_reg -> data_B_o
 *
 * with the register-file write payload tapped at data_i_d.  The A/B
 * arbiter enable is an input; its timing is owned by the strict input
 * feeder skeleton.  A disabled edge registers a zero beat exactly like
 * the RTL mux.
 */
class FeederBPipeline
{
  public:
    /** Advance one clock edge with the current mem_ctrl beat. */
    void step(const MemoryBeat256 &dataIn, bool enableB);
    /** register_file_wdata_o: the one-stage data_i_d tap. */
    const MemoryBeat256 &registerFileWriteData() const;
    /** data_B_o: the registered arbiter output, zero when disabled. */
    const MemoryBeat256 &operandB() const;

  private:
    MemoryBeat256 dataD1;
    MemoryBeat256 dataD2;
    MemoryBeat256 case0Reg;
    MemoryBeat256 operandBOut;
};

/**
 * feeder.sv operand-A payload chain for the int8 GEMM stage.  The
 * register-file readout passes through shift_register.sv, which is a
 * registered passthrough for conv_kernal<=1 (is_bypass: kernal_cnt
 * stays cleared, shift_data_result equals data_i, idle cycles load
 * zero), then through the constant conv_reuse data_A mux into the
 * registered data_A_o.  The ABTD golden confirms the payload contract:
 * the data_A value sequence equals the register-file readout sequence
 * in order; exact cycle alignment stays owned by the strict
 * input-feeder skeleton.
 */
class FeederAPipeline
{
  public:
    /** Advance one clock edge with the current register-file readout. */
    void step(const MemoryBeat256 &dataIn, bool enable);
    /** data_A_o: the registered operand-A payload. */
    const MemoryBeat256 &operandA() const;

  private:
    MemoryBeat256 shiftData;
    MemoryBeat256 operandAOut;
};

/**
 * register_file_in write-path payload composition: a pad-flagged
 * position writes zero into the padding shifter, and the shifted beat
 * is stored at the (already delay-aligned) write pointer with its
 * avail label.  The PAD_DELAY control alignment stays owned by the
 * per-tick skeletons.
 */
class InputWritePath
{
  public:
    explicit InputWritePath(uint8_t paddingBytes);

    void write(InputRegisterFile &file, uint8_t pointer,
               const MemoryBeat256 &data, bool padFlag,
               bool startOfPacket);

  private:
    StreamPaddingShifter shifter;
};

/**
 * register_file_in read-pointer program: the internal x/y/flow/ins
 * counter FSM that generates the feeder-facing read pointer from the
 * raw CSR input counter group.  Faithful source semantics: 8-bit
 * pointer arithmetic wraps mod 256, the 6-bit end compares make a raw
 * zero burst mean 64 iterations (unlike mem_addr.sv's immediate-done
 * zero guard), and each level's base address advances through the
 * shared single adder.
 */
class InputReadPointerProgram
{
  public:
    explicit InputReadPointerProgram(const SauInputCsrConfig &counters);

    bool done() const;
    uint8_t pointer() const;
    /** register_xburst_done_flag: x and y ends met (data_last source). */
    bool lastOfBurst() const;
    /** Final accepted read of the whole program. */
    bool last() const;
    uint32_t x() const;
    uint32_t y() const;
    uint32_t flow() const;
    uint32_t instruction() const;
    void advance();

  private:
    bool endX() const;
    bool endY() const;
    bool endFlow() const;
    bool endInstruction() const;

    uint32_t xLimit = 0;
    uint32_t yLimit = 0;
    uint32_t flowLimit = 0;
    uint32_t instructionLimit = 0;
    uint8_t xStep = 0;
    uint8_t yStep = 0;
    uint8_t flowStep = 0;
    uint8_t instructionStep = 0;
    uint32_t cntX = 0;
    uint32_t cntY = 0;
    uint32_t cntFlow = 0;
    uint32_t cntInstruction = 0;
    uint8_t addrCurrent = 0;
    uint8_t addrYBase = 0;
    uint8_t addrFlowBase = 0;
    uint8_t addrInstructionBase = 0;
    bool exhausted = false;
};

} // namespace gem5::sau

#endif // __SAU_INPUT_DATAPATH_HH__
