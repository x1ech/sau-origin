#include "sau/address_generator.hh"

#include <cassert>

namespace gem5::sau
{

AddressGenerator::AddressGenerator(const SauCommand &command)
    : command(command) // 拷贝 command，防止外部修改影响正在进行的地址序列
{
    updateFront(); // 初始化为第一个 A preload beat
}

bool
AddressGenerator::empty() const
{
    return exhausted; // 四重循环全部遍历完毕时被 pop() 设为 true
}

const Beat &
AddressGenerator::front() const
{
    assert(!empty());
    return current; // current 由 updateFront() 维护，保持为游标位置的最新值
}

void
AddressGenerator::pop()
{
    assert(!empty());

    // 外部 SRAM read 顺序：
    //   每个 instruction 先预装 A 一次，再按 flow stream B。
    // A 后续进入阵列的复用不再产生外部 SRAM read。

    ++beat;                               // 前进同一 stream 内的 beat
    if (beat < streamDesc().beats) {      // 还没到该 stream 最后一个 beat
        updateFront();
        return;
    }

    beat = 0;                             // beat 用完了，重置
    if (stream == StreamKind::OperandA) { // A preload 结束 → 开始 B stream
        stream = StreamKind::OperandB;
        updateFront();
        return;
    }

    // B 当前 flow 结束，继续下一个 flow；所有 flow 完成后推进 instruction。
    ++flow;
    if (flow < command.flowLoops) {       // 还有下一个 flow
        updateFront();
        return;
    }

    flow = 0;                             // flow 也完结 → 重置，推进 instruction
    ++instruction;
    if (instruction < command.instructionLoops) { // 还有下一条 instruction
        stream = StreamKind::OperandA;
        updateFront();
        return;
    }

    exhausted = true; // 所有循环都遍历完毕
}

uint64_t
AddressGenerator::totalReadBeats() const
{
    // A 每条 instruction 只从外部 SRAM preload 一次；
    // B 仍按 flow/instruction 流式读取。
    const uint64_t aBeats =
        static_cast<uint64_t>(command.operandA.beats) *
        command.instructionLoops;
    const uint64_t bBeats =
        static_cast<uint64_t>(command.operandB.beats) *
        command.flowLoops * command.instructionLoops;
    return aBeats + bBeats;
}

const StreamDesc &
AddressGenerator::streamDesc() const
{
    // 根据当前游标 stream 返回 A 或 B 的 StreamDesc，
    // 从而拿到正确的 base、stride 和 beats 字段用于地址计算
    return stream == StreamKind::OperandB ?
        command.operandB : command.operandA;
}

Addr
AddressGenerator::operandBAddress() const
{
    const auto &program = command.operandBAddress;
    if (!program.enabled) {
        return command.operandB.base +
            static_cast<Addr>(instruction) *
                command.operandB.instructionStrideBytes +
            static_cast<Addr>(flow) * command.operandB.flowStrideBytes +
            static_cast<Addr>(beat) * command.operandB.strideBytes;
    }

    uint64_t index =
        (static_cast<uint64_t>(instruction) * command.flowLoops + flow) *
            command.operandB.beats + beat;
    const uint32_t x = index % program.xCount;
    index /= program.xCount;
    const uint32_t y = index % program.yCount;
    index /= program.yCount;
    const uint32_t nestedFlow = index % program.flowCount;
    index /= program.flowCount;
    const uint32_t nestedInstruction = index % program.instructionCount;

    return command.operandB.base +
        static_cast<Addr>(x) * program.xStepBytes +
        static_cast<Addr>(y) * program.yStepBytes +
        static_cast<Addr>(nestedFlow) * program.flowStepBytes +
        static_cast<Addr>(nestedInstruction) * program.instructionStepBytes;
}

void
AddressGenerator::updateFront()
{
    // 地址公式：
    //   base + instruction * instructionStrideBytes
    //        + streamFlow * flowStrideBytes
    //        + beat * strideBytes
    //
    // A preload 不随 flow 变化；B stream 使用真实 flow 游标。
    const auto &desc = streamDesc();
    const Addr address = stream == StreamKind::OperandB ?
        operandBAddress() :
        desc.base +
            static_cast<Addr>(instruction) * desc.instructionStrideBytes +
            static_cast<Addr>(beat) * desc.strideBytes;
    current = {
        stream,
        address,
        beat,
        beat + 1 == desc.beats, // 当前 beat 是否为该 stream 出现的最后一个
    };
}

} // namespace gem5::sau
