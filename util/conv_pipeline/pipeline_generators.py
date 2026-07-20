#!/usr/bin/env python3
"""Deterministic signed data generators shared by pipeline tooling."""

from util.conv_pipeline.pipeline_contract import PipelineConfigError


def _index(value, field):
    if type(value) is not int or value < 0 or value > (1 << 64) - 1:
        raise PipelineConfigError(f"{field} must be a uint64 value")
    return value


def signed_int8(raw):
    if type(raw) is not int or raw < 0 or raw > 255:
        raise PipelineConfigError("raw activation must be in [0, 255]")
    return raw if raw < 128 else raw - 256


def activation_raw_v1(n, c, h, w):
    values = tuple(
        _index(value, field)
        for value, field in ((n, "n"), (c, "c"), (h, "h"), (w, "w"))
    )
    n, c, h, w = values
    return ((n % 256) * 97 + (c % 256) * 31 +
            (h % 256) * 7 + w % 256 + 1) % 256


def activation_value_v1(n, c, h, w):
    return signed_int8(activation_raw_v1(n, c, h, w))


def weight_value(generator, oc, c, kh, kw):
    values = tuple(
        _index(value, field)
        for value, field in (
            (oc, "oc"), (c, "c"), (kh, "kh"), (kw, "kw"))
    )
    oc, c, kh, kw = values
    if generator == "tb_weight_value_v1":
        return ((oc % 255) * 29 + (c % 255) * 17 +
                (kh % 255) * 5 + (kw % 255) * 3 + 11) % 255 - 127
    if generator == "zero":
        return 0
    if generator == "ones":
        return 1
    raise PipelineConfigError(f"unsupported weight generator: {generator}")


def bias_value(generator, oc):
    oc = _index(oc, "oc")
    if generator == "tb_bias_value_v1":
        return ((oc % 257) * 37 + 13) % 257 - 128
    if generator == "zero":
        return 0
    raise PipelineConfigError(f"unsupported bias generator: {generator}")
