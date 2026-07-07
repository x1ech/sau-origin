#ifndef __SAU_ADDRESS_GENERATOR_HH__
#define __SAU_ADDRESS_GENERATOR_HH__

#include <cstdint>

#include "sau/types.hh"

namespace gem5::sau
{

// 确定性外部 SRAM read 地址生成器。
// int8 GEMM 首个校准基线中，A 先预装入 register_file_in，
// 随后 B 从 SRAM 流式读取；A 的阵列输入复用由 ARegisterFileIn 建模。
// 使用游标式设计（不预先生成全部 beat 的 vector），
// 游标 {stream, beat, flow, instruction} 联合决定当前 front() 的值。
class AddressGenerator
{
  public:
    explicit AddressGenerator(const SauCommand &command);

    bool empty() const;              // 全部 beat 已生成完毕
    const Beat &front() const;       // 看当前 beat（不前进游标）
    void pop();                      // 当前 beat 消耗完，推进游标到下一个
    uint64_t totalReadBeats() const; // 外部 A preload + B stream 总读 beat

  private:
    // 根据当前游标 stream 返回 A 或 B 的 StreamDesc。
    const StreamDesc &streamDesc() const;
    // 用游标值 + StreamDesc 地址公式刷新 current。
    void updateFront();

    const SauCommand command;       // 命令副本，构造函数拷贝
    // 当前游标：A preload 或 B stream
    StreamKind stream = StreamKind::OperandA;
    uint32_t beat = 0;              // 当前游标：stream 内第几个 beat
    uint32_t flow = 0;              // 当前游标：第几个 flow 循环
    uint32_t instruction = 0;       // 当前游标：第几个 instruction 循环
    bool exhausted = false;         // 全部分生成完毕的标志
    Beat current;                   // 最后一拍 updateFront() 算出的 beat，供 front() 返回
};

} // namespace gem5::sau

#endif // __SAU_ADDRESS_GENERATOR_HH__
