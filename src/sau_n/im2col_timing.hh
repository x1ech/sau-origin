#ifndef __SAU_N_IM2COL_TIMING_HH__
#define __SAU_N_IM2COL_TIMING_HH__

#include "base/statistics.hh"
#include "params/Im2ColTiming.hh"
#include "sau_n/im2col_model.hh"
#include "sau_n/im2col_trace.hh"
#include "sim/clocked_object.hh"
#include "sim/eventq.hh"

namespace gem5::sau_n
{

class Im2ColTiming : public ClockedObject
{
  public:
    PARAMS(Im2ColTiming);

    explicit Im2ColTiming(const Params &params);
    void startup() override;

  private:
    void tick();
    void updateFinalStats();

    struct Im2ColStats : public statistics::Group
    {
        explicit Im2ColStats(statistics::Group *parent);

        statistics::Scalar rtlDoneCycle;
        statistics::Scalar drainedCycle;
        statistics::Scalar postDoneDrainCycles;
        statistics::Scalar totalDoneCycles;
        statistics::Scalar totalDrainedCycles;
        statistics::Scalar feedVectors;
        statistics::Scalar handshakes;
        statistics::Scalar feedVectorsPerCycle;
        statistics::Scalar presentedLanes;
        statistics::Scalar sramReadLanes;
        statistics::Scalar paddingZeroLanes;
        statistics::Scalar invalidLanes;
        statistics::Vector bankRequestCycles;
        statistics::Vector bankUtilization;
        statistics::Scalar bankRowConflicts;
        statistics::Scalar extraCollectCycles;
        statistics::Scalar fifoAverageOccupancy;
        statistics::Scalar fifoPeakOccupancy;
        statistics::Scalar fifoFullStallCycles;
        statistics::Scalar backpressureCycles;
    } stats;

    const ResolvedConfig resolved;
    Im2ColModel model;
    const PeriodicReady ready;
    Im2ColTraceWriter traceWriter;
    EventFunctionWrapper tickEvent;
};

} // namespace gem5::sau_n

#endif // __SAU_N_IM2COL_TIMING_HH__
