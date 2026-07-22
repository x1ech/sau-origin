from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class StreamingConvPipelineTiming(ClockedObject):
    type = "StreamingConvPipelineTiming"
    cxx_class = "gem5::sau_n::StreamingConvPipelineTiming"
    cxx_header = "sau_n/streaming_conv_pipeline_timing.hh"

    schema_version = Param.UInt64(1, "Resolved pipeline schema version")
    fixture_name = Param.String("", "Resolved pipeline fixture name")
    im2col_name = Param.String("", "Resolved nested Im2Col fixture name")
    n = Param.UInt64("Input batch size")
    c = Param.UInt64("Input channel count")
    h = Param.UInt64("Input height")
    w = Param.UInt64("Input width")
    out_h = Param.UInt64("Resolved output height")
    out_w = Param.UInt64("Resolved output width")
    kernel_h = Param.UInt64("Kernel height")
    kernel_w = Param.UInt64("Kernel width")
    stride_h = Param.UInt64("Vertical stride")
    stride_w = Param.UInt64("Horizontal stride")
    dilation_h = Param.UInt64("Vertical dilation")
    dilation_w = Param.UInt64("Horizontal dilation")
    pad_top = Param.UInt64("Top padding")
    pad_left = Param.UInt64("Left padding")
    spad_base = Param.UInt64("Scratchpad row base")
    cfg_dw_mode = Param.UInt64(0, "Reference RTL depthwise mode")
    cfg_kernel_pattern = Param.UInt64(
        0xFFFF, "Reference RTL kernel pattern"
    )
    input_generator = Param.String(
        "tb_act_value_v1", "Deterministic activation generator"
    )
    out_channels = Param.UInt64("Output channel count")
    cutbit = Param.UInt64("Arithmetic output right shift")
    weight_generator = Param.String("Deterministic weight generator")
    bias_generator = Param.String("Deterministic bias generator")
    resolved_config_sha256 = Param.String(
        "", "Canonical resolved pipeline configuration SHA256"
    )
    trace_file = Param.String("", "Streaming control cycle CSV path")
    output_file = Param.String("", "Final NCHW output CSV path")
    detailed_pe_trace = Param.Bool(False, "Include detailed PE trace fields")
    output_ready_period = Param.UInt64(1, "Periodic output-ready period")
    output_ready_high_cycles = Param.UInt64(
        1, "High cycles in each output-ready period"
    )
