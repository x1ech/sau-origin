#!/usr/bin/env python3
"""Frozen configuration and cycle contracts for the Im2Col timing model."""

from dataclasses import asdict, dataclass, replace
from enum import IntEnum
import hashlib
import json


SCHEMA_VERSION = 1
BLOCK_SIZE = 16
SP_BANKS = 16
ELEM_BITS = 8
SP_BANK_ENTRIES = 4096
FIFO_DEPTH = 4
KERNEL_PATTERN_ALL = 0xFFFF
UINT64_MAX = (1 << 64) - 1

REQ_VALID_HEX_DIGITS = 4
REQ_ADDR_HEX_DIGITS = 3
RESP_VALID_HEX_DIGITS = 4
RESP_DATA_HEX_DIGITS = 2
FEED_DATA_HEX_DIGITS = 32
FEED_MASK_HEX_DIGITS = 4


class ConfigError(ValueError):
    """Raised when a resolved configuration violates the frozen contract."""


class Im2ColState(IntEnum):
    IDLE = 0
    ISSUE = 1
    COLLECT = 2
    PUSH = 3
    NEXT = 4
    DONE = 5


@dataclass(frozen=True)
class ResolvedConfig:
    schema_version: int = SCHEMA_VERSION
    name: str = ""
    n: int = 0
    c: int = 0
    h: int = 0
    w: int = 0
    out_h: int = 0
    out_w: int = 0
    kernel_h: int = 0
    kernel_w: int = 0
    stride_h: int = 0
    stride_w: int = 0
    dilation_h: int = 0
    dilation_w: int = 0
    pad_top: int = 0
    pad_left: int = 0
    spad_base: int = 0
    cfg_dw_mode: int = 0
    cfg_kernel_pattern: int = KERNEL_PATTERN_ALL
    input_generator: str = "tb_act_value_v1"


@dataclass(frozen=True)
class DerivedConfig:
    rows_per_word: int
    w_words: int
    spatial_words_per_channel: int
    total_spatial_words: int
    h_groups: int
    w_groups: int
    expected_vectors: int


@dataclass(frozen=True)
class ControlRegisters:
    state: Im2ColState = Im2ColState.IDLE
    done: bool = False


@dataclass(frozen=True)
class CycleObservation:
    cycle: int
    state: Im2ColState
    busy: bool
    done: bool


TRACE_FIELDS = (
    "schema_version",
    "resolved_config_sha256",
    "cycle",
    "state",
    "busy",
    "done",
    "fifo_count",
    "fifo_rptr",
    "fifo_wptr",
    "req_valid",
    *(f"req_addr_b{bank:02d}" for bank in range(SP_BANKS)),
    "resp_valid",
    *(f"resp_data_b{bank:02d}" for bank in range(SP_BANKS)),
    "feed_valid",
    "feed_ready",
    "feed_data",
    "feed_mask",
)


def _require_integer(value, field):
    if type(value) is not int:
        raise ConfigError(f"{field} must be an integer")


def _require_range(value, minimum, maximum, field):
    _require_integer(value, field)
    if value < minimum or value > maximum:
        raise ConfigError(f"{field} must be in [{minimum}, {maximum}]")


def _require_uint64(value, description):
    if type(value) is not int or value < 0 or value > UINT64_MAX:
        raise ConfigError(f"{description} must be a uint64 value")


def checked_add(lhs, rhs, description):
    _require_uint64(lhs, description)
    _require_uint64(rhs, description)
    if lhs > UINT64_MAX - rhs:
        raise ConfigError(f"{description} overflows")
    return lhs + rhs


def checked_multiply(lhs, rhs, description):
    _require_uint64(lhs, description)
    _require_uint64(rhs, description)
    if rhs and lhs > UINT64_MAX // rhs:
        raise ConfigError(f"{description} overflows")
    return lhs * rhs


def ceil_divide(value, divisor):
    _require_uint64(value, "ceil_divide value")
    _require_uint64(divisor, "ceil_divide divisor")
    if divisor == 0:
        raise ConfigError("ceil_divide divisor must be nonzero")
    return value // divisor + (value % divisor != 0)


def validate_and_derive(config):
    """Validate an already-resolved config and return its derived counts."""
    if not isinstance(config, ResolvedConfig):
        raise ConfigError("config must be a ResolvedConfig")
    _require_range(config.schema_version, SCHEMA_VERSION, SCHEMA_VERSION,
                   "schema_version")
    if type(config.name) is not str:
        raise ConfigError("name must be a string")
    if (type(config.input_generator) is not str or
            config.input_generator != "tb_act_value_v1"):
        raise ConfigError("input_generator must be tb_act_value_v1")

    for field in ("n", "c", "h", "w", "out_h", "out_w"):
        _require_range(getattr(config, field), 1, 65535, field)
    for field in ("pad_top", "pad_left"):
        _require_range(getattr(config, field), 0, 65535, field)
    for field in (
            "kernel_h", "kernel_w", "stride_h", "stride_w",
            "dilation_h", "dilation_w"):
        _require_range(getattr(config, field), 1, 15, field)
    _require_range(config.spad_base, 0, SP_BANK_ENTRIES - 1, "spad_base")

    kernel_area = checked_multiply(
        config.kernel_h, config.kernel_w, "kernel area")
    if kernel_area > BLOCK_SIZE:
        raise ConfigError("kernel_h * kernel_w must be <= 16")
    _require_range(config.cfg_dw_mode, 0, 0, "cfg_dw_mode")
    _require_range(
        config.cfg_kernel_pattern, KERNEL_PATTERN_ALL, KERNEL_PATTERN_ALL,
        "cfg_kernel_pattern")
    if config.w <= BLOCK_SIZE and config.out_w > config.w:
        raise ConfigError("out_w must be <= w when w <= 16")

    rows_per_word = BLOCK_SIZE // config.w if config.w <= BLOCK_SIZE else 1
    w_words = ceil_divide(config.w, BLOCK_SIZE)
    spatial_words_per_channel = (
        ceil_divide(config.h, rows_per_word)
        if config.w <= BLOCK_SIZE
        else checked_multiply(config.h, w_words, "spatial word count")
    )
    total_spatial_words = checked_multiply(
        checked_multiply(config.n, config.c, "total spatial word count"),
        spatial_words_per_channel,
        "total spatial word count",
    )
    footprint_end = checked_add(
        config.spad_base, total_spatial_words, "scratchpad footprint")
    if footprint_end > SP_BANK_ENTRIES:
        raise ConfigError(
            "spad_base + total_spatial_words must be <= 4096")

    h_groups = (
        ceil_divide(config.out_h, rows_per_word)
        if config.w <= BLOCK_SIZE else config.out_h
    )
    w_groups = (
        1 if config.w <= BLOCK_SIZE
        else ceil_divide(config.out_w, BLOCK_SIZE)
    )
    expected_vectors = checked_multiply(
        config.n, config.c, "expected vector count")
    expected_vectors = checked_multiply(
        expected_vectors, config.kernel_h, "expected vector count")
    expected_vectors = checked_multiply(
        expected_vectors, config.kernel_w, "expected vector count")
    expected_vectors = checked_multiply(
        expected_vectors, h_groups, "expected vector count")
    expected_vectors = checked_multiply(
        expected_vectors, w_groups, "expected vector count")

    return DerivedConfig(
        rows_per_word=rows_per_word,
        w_words=w_words,
        spatial_words_per_channel=spatial_words_per_channel,
        total_spatial_words=total_spatial_words,
        h_groups=h_groups,
        w_groups=w_groups,
        expected_vectors=expected_vectors,
    )


def canonical_config_bytes(config):
    validate_and_derive(config)
    text = json.dumps(
        asdict(config), sort_keys=True, separators=(",", ":"),
        ensure_ascii=False,
    )
    return text.encode("utf-8")


def resolved_config_sha256(config):
    return hashlib.sha256(canonical_config_bytes(config)).hexdigest()


def is_busy(registers):
    return registers.state != Im2ColState.IDLE


def begin_next(old_registers):
    return replace(old_registers, done=False)


def commit(_old_registers, next_registers):
    return next_registers


def cycle_zero_registers():
    return ControlRegisters(state=Im2ColState.ISSUE, done=False)


def done_transition_next(old_registers):
    if old_registers.state != Im2ColState.DONE:
        raise ValueError("done transition requires old state ST_DONE")
    return ControlRegisters(state=Im2ColState.IDLE, done=True)


def observe(cycle, registers):
    _require_uint64(cycle, "cycle")
    return CycleObservation(
        cycle=cycle,
        state=registers.state,
        busy=is_busy(registers),
        done=registers.done,
    )
