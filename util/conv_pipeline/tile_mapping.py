#!/usr/bin/env python3
"""Independent spatial tile/lane mapping for the convolution pipeline."""

from dataclasses import dataclass

from util.conv_pipeline.pipeline_contract import (
    SA_ROWS,
    PipelineConfigError,
    ResolvedPipelineConfig,
    validate_and_derive,
)
from util.im2col.im2col_contract import checked_add, checked_multiply


class TileMappingError(PipelineConfigError):
    """Raised when spatial lanes cannot form the frozen SA row mapping."""


@dataclass(frozen=True)
class SpatialPosition:
    n: int
    oh: int
    ow: int


@dataclass(frozen=True)
class SpatialTile:
    index: int
    n: int
    oh_base: int
    ow_base: int
    lanes: tuple

    @property
    def valid_mask(self):
        return sum(
            1 << lane for lane, position in enumerate(self.lanes)
            if position is not None)

    @property
    def valid_rows(self):
        return sum(position is not None for position in self.lanes)


def _is_prefix_mask(mask, width=SA_ROWS):
    if type(mask) is not int or mask < 0 or mask >= 1 << width:
        return False
    return mask == (1 << bin(mask).count("1")) - 1


def _tile_lanes(config, n, oh_base, ow_base, rows_per_word):
    lanes = []
    for lane in range(SA_ROWS):
        if config.im2col.w <= SA_ROWS:
            local_h = lane // config.im2col.w
            ow = lane % config.im2col.w
            oh = oh_base + local_h
            valid = (
                local_h < rows_per_word and
                oh < config.im2col.out_h and
                ow < config.im2col.out_w
            )
        else:
            oh = oh_base
            ow = ow_base + lane
            valid = ow < config.im2col.out_w
        lanes.append(SpatialPosition(n, oh, ow) if valid else None)
    return tuple(lanes)


def spatial_tiles(config):
    """Return all SA spatial tiles and independently verify exact coverage."""
    if not isinstance(config, ResolvedPipelineConfig):
        raise TileMappingError("config must be a ResolvedPipelineConfig")
    derived = validate_and_derive(config)
    tiles = []
    for n in range(config.im2col.n):
        if config.im2col.w <= SA_ROWS:
            groups = (
                (oh_base, 0)
                for oh_base in range(
                    0, config.im2col.out_h, derived.im2col.rows_per_word)
            )
        else:
            groups = (
                (oh, ow_base)
                for oh in range(config.im2col.out_h)
                for ow_base in range(0, config.im2col.out_w, SA_ROWS)
            )
        for oh_base, ow_base in groups:
            lanes = _tile_lanes(
                config, n, oh_base, ow_base, derived.im2col.rows_per_word)
            tile = SpatialTile(
                index=len(tiles),
                n=n,
                oh_base=oh_base,
                ow_base=ow_base,
                lanes=lanes,
            )
            if tile.valid_rows == 0:
                raise TileMappingError("spatial tile must have at least one row")
            if not _is_prefix_mask(tile.valid_mask):
                raise TileMappingError(
                    f"tile {tile.index} spatial mask 0x{tile.valid_mask:04x} "
                    "is not a canonical prefix")
            tiles.append(tile)

    if len(tiles) != derived.expected_tiles:
        raise TileMappingError(
            "spatial tile count disagrees with expected_tiles")
    positions = [
        position
        for tile in tiles
        for position in tile.lanes
        if position is not None
    ]
    expected_positions = checked_multiply(
        checked_multiply(
            config.im2col.n, config.im2col.out_h,
            "spatial output count"),
        config.im2col.out_w,
        "spatial output count",
    )
    if len(positions) != expected_positions:
        raise TileMappingError(
            "spatial lane count disagrees with N*output_h*output_w")
    if len(set(positions)) != len(positions):
        raise TileMappingError("spatial mapping contains duplicate coordinates")
    return tuple(tiles)


def output_coordinate(config, tile, row, output_channel):
    """Map one accepted SA row/column to its logical NCHW coordinate."""
    validate_and_derive(config)
    if not isinstance(tile, SpatialTile):
        raise TileMappingError("tile must be a SpatialTile")
    if type(row) is not int or row < 0 or row >= SA_ROWS:
        raise TileMappingError("row must be in [0, 15]")
    if (type(output_channel) is not int or output_channel < 0 or
            output_channel >= config.out_channels):
        raise TileMappingError("output_channel is outside configured columns")
    position = tile.lanes[row]
    if position is None:
        raise TileMappingError("cannot map an invalid spatial row")
    return position.n, output_channel, position.oh, position.ow


def nchw_output_index(config, n, output_channel, oh, ow):
    """Return the checked flat index for output[n][oc][oh][ow]."""
    validate_and_derive(config)
    coordinates = (
        (n, config.im2col.n, "n"),
        (output_channel, config.out_channels, "output_channel"),
        (oh, config.im2col.out_h, "oh"),
        (ow, config.im2col.out_w, "ow"),
    )
    for value, extent, name in coordinates:
        if type(value) is not int or value < 0 or value >= extent:
            raise TileMappingError(f"{name} coordinate is out of range")
    index = checked_multiply(n, config.out_channels, "NCHW output index")
    index = checked_add(index, output_channel, "NCHW output index")
    index = checked_multiply(index, config.im2col.out_h, "NCHW output index")
    index = checked_add(index, oh, "NCHW output index")
    index = checked_multiply(index, config.im2col.out_w, "NCHW output index")
    return checked_add(index, ow, "NCHW output index")
