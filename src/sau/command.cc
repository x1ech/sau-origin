#include "sau/command.hh"

#include <array>
#include <limits>
#include <stdexcept>

namespace gem5::sau
{
namespace
{

uint32_t
checkedProduct(uint32_t lhs, uint32_t rhs, const char *description)
{
    /* 仿真层安全检查：将乘法提升至64位避免溢出回绕。
     * RTL 无此机制 —— 硬件计数器以固定位宽自旋，溢出是其自然行为。
     * 此处仅防止用户非法配置导致模型静默异常。 */

    const auto product = static_cast<uint64_t>(lhs) * rhs;
    if (product > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(description);
    }
    return static_cast<uint32_t>(product);
}

void
validateStream(const StreamDesc &stream, unsigned beatBytes)
{
    //非法拦截
    if (stream.beats == 0) {
        throw std::invalid_argument(
            "SAU stream must contain at least one beat");
    }
    if (stream.strideBytes == 0) {
        throw std::invalid_argument("SAU stream stride must be nonzero");
    }
    if (stream.base % beatBytes != 0) {
        throw std::invalid_argument("SAU stream base must be beat aligned");
    }
}

} // anonymous namespace

uint32_t
effectiveScheduleInstructions(const SauCommand &command)
{
    if (command.scheduleInstructions != 0) {
        return command.scheduleInstructions;
    }
    return checkedProduct(
        command.flowLoops, command.instructionLoops,
        "SAU schedule instruction count exceeds its representation");
}

void
validateCommand(const SauCommand &command, unsigned beatBytes)
{
    if (beatBytes != 32) {
        throw std::invalid_argument(
            "first SAU milestone requires 32-byte beats");
    }
    if (command.operation != Operation::Gemm) {
        throw std::invalid_argument("first SAU milestone supports GEMM only");
    }
    if (command.precision != Precision::Int8) {
        throw std::invalid_argument("first SAU milestone supports int8 only");
    }

    const std::array<const StreamDesc *, 3> streams = {
        &command.operandA,
        &command.operandB,
        &command.output,
    };
    for (const auto *stream : streams) {
        validateStream(*stream, beatBytes);
    }

    if (command.flowLoops == 0) {
        throw std::invalid_argument("SAU flow loop count must be nonzero");
    }
    if (command.instructionLoops == 0) {
        throw std::invalid_argument(
            "SAU instruction loop count must be nonzero");
    }
    if (command.workItems == 0) {
        throw std::invalid_argument("SAU work item count must be nonzero");
    }

    auto expectedWorkItems = checkedProduct(
        command.operandB.beats, command.flowLoops,
        "SAU work item count exceeds its representation");
    expectedWorkItems = checkedProduct(
        expectedWorkItems, command.instructionLoops,
        "SAU work item count exceeds its representation");
    if (command.workItems != expectedWorkItems) {
        throw std::invalid_argument(
            "SAU work item count does not match operand B");
    }

    const uint32_t scheduleInstructions =
        effectiveScheduleInstructions(command);
    if (scheduleInstructions == 0 ||
        command.workItems % scheduleInstructions != 0) {
        throw std::invalid_argument(
            "SAU work items must divide evenly across schedule instructions");
    }

    const auto expectedOutputBeats = checkedProduct(
        command.output.beats, command.instructionLoops,
        "SAU output beat count exceeds its representation");
    if (expectedOutputBeats > command.workItems) {
        throw std::invalid_argument(
            "SAU output beat count exceeds work items");
    }
    if (expectedOutputBeats % scheduleInstructions != 0) {
        throw std::invalid_argument(
            "SAU output beats must divide evenly across schedule instructions");
    }

    if (command.operandBAddress.enabled) {
        const auto &program = command.operandBAddress;
        if (program.xCount == 0 || program.yCount == 0 ||
            program.flowCount == 0 || program.instructionCount == 0) {
            throw std::invalid_argument(
                "SAU nested Operand-B address counts must be nonzero");
        }
        auto nestedBeats = checkedProduct(
            program.xCount, program.yCount,
            "SAU nested Operand-B address count overflows");
        nestedBeats = checkedProduct(
            nestedBeats, program.flowCount,
            "SAU nested Operand-B address count overflows");
        nestedBeats = checkedProduct(
            nestedBeats, program.instructionCount,
            "SAU nested Operand-B address count overflows");
        if (nestedBeats != expectedWorkItems) {
            throw std::invalid_argument(
                "SAU nested Operand-B address count does not match work");
        }
        const std::array<uint32_t, 4> steps = {
            program.xStepBytes, program.yStepBytes,
            program.flowStepBytes, program.instructionStepBytes};
        for (const auto step : steps) {
            if (step % beatBytes != 0) {
                throw std::invalid_argument(
                    "SAU nested Operand-B address step must be beat aligned");
            }
        }
    }
}

} // namespace gem5::sau
