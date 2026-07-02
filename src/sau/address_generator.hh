#ifndef __SAU_ADDRESS_GENERATOR_HH__
#define __SAU_ADDRESS_GENERATOR_HH__

#include <cstdint>

#include "sau/types.hh"

namespace gem5::sau
{

class AddressGenerator
{
  public:
    explicit AddressGenerator(const SauCommand &command);

    bool empty() const;
    const Beat &front() const;
    void pop();
    uint64_t totalReadBeats() const;

  private:
    const StreamDesc &streamDesc() const;
    void updateFront();

    const SauCommand command;
    StreamKind stream = StreamKind::OperandB;
    uint32_t beat = 0;
    uint32_t flow = 0;
    uint32_t instruction = 0;
    bool exhausted = false;
    Beat current;
};

} // namespace gem5::sau

#endif // __SAU_ADDRESS_GENERATOR_HH__
