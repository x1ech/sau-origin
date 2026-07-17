from m5.objects.ClockedObject import ClockedObject
from m5.params import *


class Im2ColTiming(ClockedObject):
    type = "Im2ColTiming"
    cxx_class = "gem5::sau_n::Im2ColTiming"
    cxx_header = "sau_n/im2col_timing.hh"

    schema_version = Param.UInt64(1, "Resolved fixture schema version")
    fixture_name = Param.String("", "Resolved fixture name")
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
    resolved_config_sha256 = Param.String(
        "", "Canonical resolved configuration SHA256"
    )
    trace_file = Param.String("", "Per-cycle Im2Col CSV trace path")

    ready_period = Param.UInt64(1, "Periodic feed-ready period")
    ready_high_cycles = Param.UInt64(
        1, "High cycles in each feed-ready period"
    )
