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
    const auto product = static_cast<uint64_t>(lhs) * rhs;
    if (product > std::numeric_limits<uint32_t>::max()) {
        throw std::invalid_argument(description);
    }
    return static_cast<uint32_t>(product);
}

void
validateStream(const StreamDesc &stream, unsigned beatBytes)
{
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

void
validateCommand(const SauCommand &command, unsigned beatBytes)
{
    if (beatBytes != 16) {
        throw std::invalid_argument(
            "first SAU milestone requires 16-byte beats");
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
        command.operandA.beats, command.flowLoops,
        "SAU work item count exceeds its representation");
    expectedWorkItems = checkedProduct(
        expectedWorkItems, command.instructionLoops,
        "SAU work item count exceeds its representation");
    if (command.workItems != expectedWorkItems) {
        throw std::invalid_argument(
            "SAU work item count does not match operand A");
    }

    const auto expectedOutputBeats = checkedProduct(
        command.output.beats, command.instructionLoops,
        "SAU output beat count exceeds its representation");
    if (expectedOutputBeats != command.workItems) {
        throw std::invalid_argument(
            "SAU output beat count does not match work items");
    }
}

} // namespace gem5::sau
