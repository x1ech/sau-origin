#ifndef __SAU_A_REGISTER_FILE_HH__
#define __SAU_A_REGISTER_FILE_HH__

#include <cstdint>
#include <vector>

#include "sau/types.hh"

namespace gem5::sau
{

// 抽象的 A-side register_file_in。
//
// 它不建模 RTL 寄存器位宽或端口细节，只表达体系结构相关事实：
// Operand-A 先从外部 SRAM preload，随后阵列输入端可重复读取已驻留的 A。
class ARegisterFileIn
{
  public:
    explicit ARegisterFileIn(const SauCommand &command,
                             uint32_t totalArrayInputs = 0);

    void load(const Beat &beat);

    bool instructionReady(uint32_t instruction) const;
    uint32_t loadedBeats(uint32_t instruction) const;

    uint64_t totalExternalLoadBeats() const;
    uint64_t totalArrayInputBeats() const;

    Beat arrayInputBeat(uint32_t instruction, uint32_t flow, uint32_t beat,
                        uint32_t arrayIndex) const;

  private:
    size_t slot(uint32_t instruction, uint32_t beat) const;
    void checkInstruction(uint32_t instruction) const;

    const SauCommand command;
    const uint32_t arrayInputs;
    std::vector<bool> loaded;
};

} // namespace gem5::sau

#endif // __SAU_A_REGISTER_FILE_HH__
