#!/usr/bin/env python3
"""Independent direct-NCHW functional oracle for the SAU pipeline."""

from dataclasses import dataclass

from util.conv_pipeline.pipeline_contract import (
    PipelineConfigError,
    ResolvedPipelineConfig,
    validate_and_derive,
)
from util.conv_pipeline.pipeline_generators import (
    activation_value_v1,
    bias_value,
    weight_value,
)


ACC24_MIN = -(1 << 23)
ACC24_MAX = (1 << 23) - 1
INT8_MIN = -128
INT8_MAX = 127


class OracleError(PipelineConfigError):
    """Raised when independent oracle generation violates its contract."""


@dataclass(frozen=True)
class OracleOutput:
    n: int
    output_channel: int
    oh: int
    ow: int
    accumulator: int
    value: int

    @property
    def rtl_slot(self):
        return self.value & 0xffff


@dataclass(frozen=True)
class OracleResult:
    outputs: tuple
    mac_count: int

    @property
    def values(self):
        return tuple(output.value for output in self.outputs)


def saturating_add_signed_24(accumulator, addend):
    if type(accumulator) is not int or type(addend) is not int:
        raise OracleError("24-bit saturating add operands must be integers")
    if accumulator < ACC24_MIN or accumulator > ACC24_MAX:
        raise OracleError("accumulator must already be a signed 24-bit value")
    total = accumulator + addend
    return min(ACC24_MAX, max(ACC24_MIN, total))


def quantize_accumulator(accumulator, cutbit):
    if type(accumulator) is not int or not ACC24_MIN <= accumulator <= ACC24_MAX:
        raise OracleError("accumulator must be a signed 24-bit value")
    if type(cutbit) is not int or not 0 <= cutbit <= 23:
        raise OracleError("cutbit must be in [0, 23]")
    shifted = accumulator >> cutbit
    return min(INT8_MAX, max(INT8_MIN, shifted))


def _activation(config, n, c, oh, ow, kh, kw):
    input_h = (
        oh * config.im2col.stride_h + kh * config.im2col.dilation_h -
        config.im2col.pad_top
    )
    input_w = (
        ow * config.im2col.stride_w + kw * config.im2col.dilation_w -
        config.im2col.pad_left
    )
    if (input_h < 0 or input_w < 0 or input_h >= config.im2col.h or
            input_w >= config.im2col.w):
        return 0
    return activation_value_v1(n, c, input_h, input_w)


def generate_convolution(config):
    """Generate NCHW output directly, without any tile or RTL helper."""
    if not isinstance(config, ResolvedPipelineConfig):
        raise OracleError("config must be a ResolvedPipelineConfig")
    derived = validate_and_derive(config)
    outputs = []
    mac_count = 0
    for n in range(config.im2col.n):
        for oc in range(config.out_channels):
            for oh in range(config.im2col.out_h):
                for ow in range(config.im2col.out_w):
                    accumulator = 0
                    for c in range(config.im2col.c):
                        for kh in range(config.im2col.kernel_h):
                            for kw in range(config.im2col.kernel_w):
                                activation = _activation(
                                    config, n, c, oh, ow, kh, kw)
                                weight = weight_value(
                                    config.weight_generator, oc, c, kh, kw)
                                accumulator = saturating_add_signed_24(
                                    accumulator, activation * weight)
                                mac_count += 1
                    accumulator = saturating_add_signed_24(
                        accumulator,
                        bias_value(config.bias_generator, oc),
                    )
                    outputs.append(OracleOutput(
                        n=n,
                        output_channel=oc,
                        oh=oh,
                        ow=ow,
                        accumulator=accumulator,
                        value=quantize_accumulator(
                            accumulator, config.cutbit),
                    ))
    if len(outputs) != derived.expected_outputs:
        raise OracleError("oracle output count disagrees with expected_outputs")
    if mac_count != derived.expected_macs:
        raise OracleError("oracle MAC count disagrees with expected_macs")
    return OracleResult(outputs=tuple(outputs), mac_count=mac_count)
