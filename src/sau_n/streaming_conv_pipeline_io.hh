#ifndef __SAU_N_STREAMING_CONV_PIPELINE_IO_HH__
#define __SAU_N_STREAMING_CONV_PIPELINE_IO_HH__

#include <array>
#include <fstream>
#include <string>
#include <string_view>

#include "sau_n/streaming_conv_pipeline_model.hh"

namespace gem5::sau_n
{

inline constexpr std::array<std::string_view, 87>
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
    "a_request_mask", "a_grant_mask", "a_response_mask",
    "a_request_tile", "a_request_k", "a_response_tile", "a_response_k",
    "b_request_mask", "b_grant_mask", "b_response_mask",
    "b_request_buffer", "b_request_slot", "b_request_k",
    "b_response_buffer", "b_response_slot", "b_response_k",
    "c_request_mask", "c_grant_mask", "c_response_mask",
    "c_request_byte", "c_response_byte",
    "d_queue_occupancy", "d_head_pending_mask",
    "d_request_mask", "d_grant_mask", "d_head_will_retire",
    "d_enqueue", "d_dequeue",
    "b_entry_hit", "b_reuse_hit", "active_b_buffer",
    "next_expected_k", "b_ready_entries",
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
