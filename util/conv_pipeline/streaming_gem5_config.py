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
    shared = loaded.config.shared_spad
    params.update({
        "spad_a_base": shared.a_base,
        "spad_a_rows": shared.a_rows,
        "spad_b_base": shared.b_base,
        "spad_b_rows": shared.b_rows,
        "spad_c_base": shared.c_base,
        "spad_c_rows": shared.c_rows,
        "spad_d_base": shared.d_base,
        "spad_d_rows": shared.d_rows,
        "b_buffer_depth": shared.b_buffer_depth,
        "d_pending_rows": shared.d_pending_rows,
        "weight_reuse": shared.weight_reuse,
        "bank_arbitration": shared.arbitration,
    })
    return params
