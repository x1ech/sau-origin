#ifndef __SAU_N_STREAMING_CONV_PIPELINE_IO_HH__
#define __SAU_N_STREAMING_CONV_PIPELINE_IO_HH__

#include <array>
#include <fstream>
#include <string>
#include <string_view>

#include "sau_n/streaming_conv_pipeline_model.hh"

namespace gem5::sau_n
{

inline constexpr std::array<std::string_view, 54>
StreamingPipelineTraceFields = {
    "schema_version", "resolved_config_sha256", "cycle",
    "s0_valid", "s0_ready", "s0_fire", "s0_tile", "s0_k",
    "s1_valid", "s1_can_retire", "s1_ready", "s1_fire",
    "s1_tile", "s1_k", "s1_lane_done", "s1_request_valid",
    "s1_request_rows", "s1_read_rounds", "s1_raw_spatial_mask",
    "s2_valid", "s2_ready", "s2_fire", "s2_tile", "s2_k",
    "s2_compacted_spatial_mask", "s2_source_lanes",
    "producer_fire", "producer_exhausted", "fifo_count", "fifo_rptr",
    "fifo_wptr", "fifo_push_ready", "fifo_push", "fifo_pop",
    "fifo_head_valid", "fifo_head_tile", "fifo_head_k",
    "consumer_state", "active_tile", "accepted_k", "pe_ready",
    "begin_launch", "launch", "input_valid", "input_fire", "sa_state",
    "sa_input_valid", "output_grant", "storage_ready", "row_score_valid",
    "row_sequence", "cal_finish", "output_collected", "drained",
};

inline constexpr std::array<std::string_view, 6>
StreamingPipelineDetailedTraceFields = {
    "pe_valid_mask", "pe_mac_commit_mask", "pe_add_commit_mask",
    "pe_activations", "pe_weights", "pe_accumulators",
};

class StreamingConvPipelineTraceWriter
{
  public:
    StreamingConvPipelineTraceWriter(
        const std::string &path, const std::string &resolvedConfigSha256,
        bool detailedPeTrace);
    void emit(const StreamingConvPipelineCycle &cycle);

  private:
    bool enabled = false;
    bool detailed = false;
    std::string configSha256;
    std::ofstream output;
};

} // namespace gem5::sau_n

#endif // __SAU_N_STREAMING_CONV_PIPELINE_IO_HH__
