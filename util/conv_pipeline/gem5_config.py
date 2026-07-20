#!/usr/bin/env python3
"""Convert a loaded pipeline fixture into ConvPipelineTiming parameters."""

from pathlib import Path

from util.conv_pipeline.pipeline_contract import (
    OutputReadyConfig,
    validate_output_ready,
)


def simobject_parameters(
        loaded, trace_path, output_path, output_ready_period=1,
        output_ready_high_cycles=1):
    ready = OutputReadyConfig(
        period=output_ready_period,
        high_cycles=output_ready_high_cycles,
    )
    validate_output_ready(ready)
    if not trace_path:
        raise ValueError("trace path must not be empty")
    if not output_path:
        raise ValueError("output path must not be empty")
    trace_path = Path(trace_path)
    output_path = Path(output_path)

    config = loaded.config
    im2col = config.im2col
    return {
        "schema_version": config.schema_version,
        "fixture_name": config.name,
        "im2col_name": im2col.name,
        "n": im2col.n,
        "c": im2col.c,
        "h": im2col.h,
        "w": im2col.w,
        "out_h": im2col.out_h,
        "out_w": im2col.out_w,
        "kernel_h": im2col.kernel_h,
        "kernel_w": im2col.kernel_w,
        "stride_h": im2col.stride_h,
        "stride_w": im2col.stride_w,
        "dilation_h": im2col.dilation_h,
        "dilation_w": im2col.dilation_w,
        "pad_top": im2col.pad_top,
        "pad_left": im2col.pad_left,
        "spad_base": im2col.spad_base,
        "cfg_dw_mode": im2col.cfg_dw_mode,
        "cfg_kernel_pattern": im2col.cfg_kernel_pattern,
        "input_generator": im2col.input_generator,
        "out_channels": config.out_channels,
        "cutbit": config.cutbit,
        "weight_generator": config.weight_generator,
        "bias_generator": config.bias_generator,
        "resolved_config_sha256": loaded.resolved_config_sha256,
        "trace_file": str(trace_path),
        "output_file": str(output_path),
        "output_ready_period": ready.period,
        "output_ready_high_cycles": ready.high_cycles,
    }
