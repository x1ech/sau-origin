#ifndef __SAU_N_IM2COL_TRACE_HH__
#define __SAU_N_IM2COL_TRACE_HH__

#include <fstream>
#include <string>

#include "sau_n/im2col_model.hh"

namespace gem5::sau_n
{

class Im2ColTraceWriter
{
  public:
    Im2ColTraceWriter(
        const std::string &path, const std::string &resolvedConfigSha256);

    bool enabled() const;
    void emit(const Im2ColCycle &cycle);
    void flush();

  private:
    std::ofstream output;
    std::string configSha256;
};

} // namespace gem5::sau_n

#endif // __SAU_N_IM2COL_TRACE_HH__
