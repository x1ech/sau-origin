#ifndef __SAU_N_CONV_PIPELINE_IO_HH__
#define __SAU_N_CONV_PIPELINE_IO_HH__

#include <array>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "sau_n/conv_pipeline_model.hh"

namespace gem5::sau_n
{

inline constexpr unsigned PipelineFeedDataHexDigits = 32;
inline constexpr unsigned PipelineFeedMaskHexDigits = 4;
inline constexpr unsigned PipelineInputDataHexDigits = 32;
inline constexpr unsigned PipelineBiasHexDigits = 64;
inline constexpr unsigned PipelinePeMaskHexDigits = 64;
inline constexpr unsigned PipelinePeInt8HexDigits = 512;
inline constexpr unsigned PipelinePeAccumulatorHexDigits = 1536;
inline constexpr unsigned PipelineOutputSlotsHexDigits = 64;

inline constexpr std::array<std::string_view, 53>
CanonicalPipelineTraceFields = {
    "schema_version",
    "resolved_config_sha256",
    "cycle",
    "pipeline_state",
    "tile_index",
    "tile_buffer_count",
    "collect_k",
    "stream_k",
    "im2col_state",
    "im2col_done",
    "im2col_fifo_count",
    "im2col_fifo_rptr",
    "im2col_fifo_wptr",
    "im2col_feed_valid",
    "im2col_feed_ready",
    "im2col_feed_handshake",
    "im2col_feed_data",
    "im2col_feed_mask",
    "sa_ins_valid",
    "sa_calc_cycles",
    "sa_valid_rows",
    "sa_valid_columns",
    "sa_cutbit",
    "sa_biases",
    "sa_state",
    "sa_datain_count",
    "sa_output_counter",
    "sa_input_valid",
    "sa_activations",
    "sa_weights",
    "sa_row_mask",
    "sa_column_mask",
    "pe_valid_mask",
    "pe_mac_commit_mask",
    "pe_add_commit_mask",
    "pe_activations",
    "pe_weights",
    "pe_accumulators",
    "os_valid_mask",
    "row_ready_mask",
    "pe_finish",
    "storage_ready",
    "output_request",
    "output_grant",
    "internal_output_valid",
    "engine_output_fire",
    "row_score_valid",
    "row_sequence",
    "output_slots",
    "cal_finish",
    "output_collected",
    "sau_last_result",
    "drained",
};

class ConvPipelineTraceWriter
{
  public:
    ConvPipelineTraceWriter(
        const std::string &path, const std::string &resolvedConfigSha256);
    void emit(const ConvPipelineCycle &cycle);

  private:
    bool enabled = false;
    std::string configSha256;
    std::ofstream output;
};

void writeConvPipelineOutput(
    const std::string &path,
    const PipelineResolvedConfig &config,
    const std::vector<int8_t> &values);

} // namespace gem5::sau_n

#endif // __SAU_N_CONV_PIPELINE_IO_HH__
