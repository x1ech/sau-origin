#!/usr/bin/env python3
"""Independent logical feed-sequence oracle for the reference Im2Col RTL."""

from dataclasses import dataclass

from util.im2col.im2col_contract import (
    BLOCK_SIZE,
    ceil_divide,
    checked_multiply,
    validate_and_derive,
)


@dataclass(frozen=True)
class LogicalFeedVector:
    n: int
    c: int
    oh_base: int
    ow_base: int
    kh: int
    kw: int
    lane_data: tuple
    feed_mask: int

    @property
    def feed_data(self):
        packed = 0
        for lane, value in enumerate(self.lane_data):
            packed |= value << (lane * 8)
        return packed


def tb_act_value_v1(n, c, h, w):
    """Return the deterministic low-eight-bit activation value."""
    return (n * 97 + c * 31 + h * 7 + w + 1) & 0xFF


def expected_vector_count(config):
    """Calculate logical vector count without physical bank/row helpers."""
    validate_and_derive(config)
    rows_per_word = BLOCK_SIZE // config.w if config.w <= BLOCK_SIZE else 1
    h_groups = (
        ceil_divide(config.out_h, rows_per_word)
        if config.w <= BLOCK_SIZE else config.out_h
    )
    w_groups = (
        1 if config.w <= BLOCK_SIZE
        else ceil_divide(config.out_w, BLOCK_SIZE)
    )
    count = checked_multiply(config.n, config.c, "logical vector count")
    count = checked_multiply(
        count, config.kernel_h, "logical vector count")
    count = checked_multiply(
        count, config.kernel_w, "logical vector count")
    count = checked_multiply(count, h_groups, "logical vector count")
    return checked_multiply(count, w_groups, "logical vector count")


def _output_groups(config):
    if config.w <= BLOCK_SIZE:
        rows_per_word = BLOCK_SIZE // config.w
        for oh_base in range(0, config.out_h, rows_per_word):
            yield oh_base, 0
    else:
        for oh_base in range(config.out_h):
            for ow_base in range(0, config.out_w, BLOCK_SIZE):
                yield oh_base, ow_base


def _logical_lane(config, oh_base, ow_base, kh, kw, lane):
    if config.w <= BLOCK_SIZE:
        rows_per_word = BLOCK_SIZE // config.w
        local_h = lane // config.w
        local_w = lane % config.w
        out_h = oh_base + local_h
        out_w = local_w
        lane_shape_valid = local_h < rows_per_word
    else:
        out_h = oh_base
        out_w = ow_base + lane
        lane_shape_valid = True

    if (not lane_shape_valid or out_h >= config.out_h or
            out_w >= config.out_w):
        return False, None, None

    real_h = (
        out_h * config.stride_h + kh * config.dilation_h -
        config.pad_top
    )
    real_w = (
        out_w * config.stride_w + kw * config.dilation_w -
        config.pad_left
    )
    if real_h < 0 or real_w < 0 or real_h >= config.h or real_w >= config.w:
        return True, None, None
    return True, real_h, real_w


def iter_logical_feed_vectors(config):
    """Yield feed vectors in kw->kh->c->group->n advancement order."""
    validate_and_derive(config)
    for n_idx in range(config.n):
        for oh_base, ow_base in _output_groups(config):
            for c_idx in range(config.c):
                for kh_idx in range(config.kernel_h):
                    for kw_idx in range(config.kernel_w):
                        lane_data = [0] * BLOCK_SIZE
                        feed_mask = 0
                        for lane in range(BLOCK_SIZE):
                            presented, real_h, real_w = _logical_lane(
                                config, oh_base, ow_base,
                                kh_idx, kw_idx, lane)
                            if not presented:
                                continue
                            feed_mask |= 1 << lane
                            if real_h is not None:
                                lane_data[lane] = tb_act_value_v1(
                                    n_idx, c_idx, real_h, real_w)
                        yield LogicalFeedVector(
                            n=n_idx,
                            c=c_idx,
                            oh_base=oh_base,
                            ow_base=ow_base,
                            kh=kh_idx,
                            kw=kw_idx,
                            lane_data=tuple(lane_data),
                            feed_mask=feed_mask,
                        )
