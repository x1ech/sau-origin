#!/usr/bin/env python3
"""Convert a streaming fixture into StreamingConvPipelineTiming params."""

from util.conv_pipeline.gem5_config import simobject_parameters


def streaming_simobject_parameters(
        loaded, trace_path, output_path, output_ready_period=1,
        output_ready_high_cycles=1, detailed_pe_trace=False):
    if type(detailed_pe_trace) is not bool:
        raise ValueError("detailed_pe_trace must be a bool")
    params = simobject_parameters(
        loaded,
        trace_path,
        output_path,
        output_ready_period,
        output_ready_high_cycles,
    )
    params["detailed_pe_trace"] = detailed_pe_trace
    return params
