#!/usr/bin/env python3
"""Verify streaming pipeline artifacts with independent Python oracles."""

import argparse
import csv
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.conv_pipeline.convolution_oracle import generate_convolution
from util.conv_pipeline.streaming_fixture import load_streaming_fixture


TRACE_FIELDS = (
    "schema_version", "resolved_config_sha256", "cycle",
    "s0_valid", "s0_ready", "s0_fire", "s0_tile", "s0_k",
    "s1_valid", "s1_can_retire", "s1_ready", "s1_fire",
    "s1_tile", "s1_k", "s1_lane_done", "s1_request_valid",
    "s1_request_rows", "s1_read_rounds", "s1_raw_spatial_mask",
    "s2_valid", "s2_ready", "s2_fire", "s2_tile", "s2_k",
    "s2_compacted_spatial_mask", "s2_source_lanes",
    "producer_fire", "producer_exhausted", "fifo_count", "fifo_rptr",
    "fifo_wptr", "fifo_push_ready", "fifo_push", "fifo_pop",
    "fifo_head_valid", "fifo_head_tile", "fifo_head_k",
    "consumer_state", "active_tile", "accepted_k", "pe_ready",
    "begin_launch", "launch", "input_valid", "input_fire", "sa_state",
    "sa_input_valid", "output_grant", "storage_ready",
    "row_score_valid", "row_sequence", "cal_finish",
    "output_collected", "drained",
)

BOOL_FIELDS = (
    "s0_valid", "s0_ready", "s0_fire", "s1_valid",
    "s1_can_retire", "s1_ready", "s1_fire", "s2_valid", "s2_ready",
    "s2_fire", "producer_fire", "producer_exhausted",
    "fifo_push_ready", "fifo_push", "fifo_pop", "fifo_head_valid",
    "pe_ready", "begin_launch", "launch", "input_valid", "input_fire",
    "sa_input_valid", "output_grant", "storage_ready",
    "row_score_valid", "cal_finish", "output_collected", "drained",
)


class StreamingVerificationError(ValueError):
    """Raised when generated streaming artifacts violate the contract."""


def _fail(message):
    raise StreamingVerificationError(message)


def _integer(text, field, minimum=0):
    try:
        value = int(text, 0)
    except (TypeError, ValueError) as error:
        raise StreamingVerificationError(
            f"{field} must be an integer: {text!r}") from error
    if value < minimum:
        _fail(f"{field} must be at least {minimum}: {value}")
    return value


def _boolean(row, field, cycle):
    value = _integer(row[field], f"cycle {cycle} {field}")
    if value not in (0, 1):
        _fail(f"cycle {cycle} {field} must be 0 or 1")
    return bool(value)


def expected_tile_source_lanes(config):
    """Derive raw valid lanes directly from output geometry."""
    rows_per_word = 16 // config.im2col.w if config.im2col.w <= 16 else 1
    result = []
    for n in range(config.im2col.n):
        if config.im2col.w <= 16:
            groups = (
                (oh_base, 0)
                for oh_base in range(0, config.im2col.out_h, rows_per_word)
            )
        else:
            groups = (
                (oh, ow_base)
                for oh in range(config.im2col.out_h)
                for ow_base in range(0, config.im2col.out_w, 16)
            )
        for oh_base, ow_base in groups:
            sources = []
            coordinates = []
            for lane in range(16):
                if config.im2col.w <= 16:
                    local_h = lane // config.im2col.w
                    out_h = oh_base + local_h
                    out_w = lane % config.im2col.w
                    shape_valid = local_h < rows_per_word
                else:
                    out_h = oh_base
                    out_w = ow_base + lane
                    shape_valid = True
                if (shape_valid and out_h < config.im2col.out_h and
                        out_w < config.im2col.out_w):
                    sources.append(lane)
                    coordinates.append((n, out_h, out_w))
            if not sources:
                _fail("independent tile oracle produced an empty tile")
            result.append((tuple(sources), tuple(coordinates)))
    return tuple(result)


def _read_output(path, loaded):
    with Path(path).open(newline="", encoding="utf-8") as stream:
        reader = csv.reader(stream)
        try:
            header = next(reader)
        except StopIteration:
            _fail("output CSV is empty")
        if header != ["n", "oc", "oh", "ow", "value"]:
            _fail(f"unexpected output header: {header}")
        generated = []
        for line, row in enumerate(reader, start=2):
            if len(row) != 5:
                _fail(f"output line {line} must contain five fields")
            generated.append(tuple(
                _integer(value, f"output line {line} field {index}", -128)
                for index, value in enumerate(row)
            ))

    oracle = generate_convolution(loaded.config)
    expected = [
        (item.n, item.output_channel, item.oh, item.ow, item.value)
        for item in oracle.outputs
    ]
    if generated != expected:
        mismatch = next(
            (index for index, pair in enumerate(zip(generated, expected))
             if pair[0] != pair[1]),
            min(len(generated), len(expected)),
        )
        actual = generated[mismatch] if mismatch < len(generated) else None
        wanted = expected[mismatch] if mismatch < len(expected) else None
        _fail(
            f"output differs from direct convolution oracle at {mismatch}: "
            f"generated={actual}, expected={wanted}"
        )


def _read_stats(path):
    result = {}
    marker = ".streamingPipeline."
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) < 2 or marker not in fields[0]:
            continue
        name = fields[0].split(marker, 1)[1]
        result[name] = fields[1]
    if not result:
        _fail("stats file contains no streamingPipeline statistics")
    return result


def _stat(stats, name):
    if name not in stats:
        _fail(f"missing streamingPipeline.{name}")
    return _integer(stats[name], f"streamingPipeline.{name}")


def _decode_source_lanes(text, cycle):
    if not text.startswith("0x") or len(text) != 34:
        _fail(f"cycle {cycle} s2_source_lanes has noncanonical width")
    value = _integer(text, f"cycle {cycle} s2_source_lanes")
    return tuple((value >> (row * 8)) & 0xff for row in range(16))


def _read_trace(path, loaded):
    with Path(path).open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != TRACE_FIELDS:
            _fail("streaming trace does not use the 54-field compact schema")
        rows = list(reader)
    if not rows:
        _fail("streaming trace contains no cycle rows")

    config = loaded.config
    derived = loaded.derived
    tile_oracle = expected_tile_source_lanes(config)
    if len(tile_oracle) != derived.expected_tiles:
        _fail("independent tile count disagrees with resolved derivation")

    s1_fires = 0
    s2_fires = 0
    producer_fires = 0
    fifo_pushes = 0
    fifo_pops = 0
    pe_inputs = 0
    launches = 0
    output_rows = 0
    saw_full_exchange = False
    saw_input_bubble = False
    saw_output_stall = False
    saw_scattered = False

    for cycle, row in enumerate(rows):
        actual_cycle = _integer(row["cycle"], f"trace row {cycle} cycle")
        if actual_cycle != cycle:
            _fail(
                f"trace cycle discontinuity: expected {cycle}, "
                f"got {actual_cycle}")
        if row["schema_version"] != "1":
            _fail(f"cycle {cycle} has unexpected schema_version")
        if row["resolved_config_sha256"] != loaded.resolved_config_sha256:
            _fail(f"cycle {cycle} resolved config SHA256 mismatch")
        flags = {field: _boolean(row, field, cycle) for field in BOOL_FIELDS}
        fifo_count = _integer(row["fifo_count"], f"cycle {cycle} fifo_count")
        if fifo_count > 4:
            _fail(f"cycle {cycle} FIFO count exceeds depth four")
        saw_full_exchange |= (
            fifo_count == 4 and flags["fifo_push"] and flags["fifo_pop"])
        saw_input_bubble |= (
            _integer(row["consumer_state"], "consumer_state") == 2 and
            _integer(row["accepted_k"], "accepted_k") < derived.k and
            not flags["input_fire"]
        )
        saw_output_stall |= (
            _integer(row["consumer_state"], "consumer_state") == 4 and
            not flags["output_grant"])

        if flags["s1_fire"]:
            expected_tile = s1_fires // derived.k
            expected_k = s1_fires % derived.k
            if (_integer(row["s1_tile"], "s1_tile") != expected_tile or
                    _integer(row["s1_k"], "s1_k") != expected_k):
                _fail(f"cycle {cycle} S1 fire tag is not canonical")
            sources = tile_oracle[expected_tile][0]
            raw_mask = sum(1 << source for source in sources)
            if _integer(row["s1_raw_spatial_mask"], "raw mask") != raw_mask:
                _fail(f"cycle {cycle} S1 raw spatial mask mismatch")
            s1_fires += 1

        if flags["s2_fire"]:
            expected_tile = s2_fires // derived.k
            expected_k = s2_fires % derived.k
            if (_integer(row["s2_tile"], "s2_tile") != expected_tile or
                    _integer(row["s2_k"], "s2_k") != expected_k):
                _fail(f"cycle {cycle} S2 fire tag is not canonical")
            sources = tile_oracle[expected_tile][0]
            compacted_mask = (1 << len(sources)) - 1
            if _integer(
                    row["s2_compacted_spatial_mask"],
                    "compacted mask") != compacted_mask:
                _fail(f"cycle {cycle} S2 compacted mask mismatch")
            decoded = _decode_source_lanes(row["s2_source_lanes"], cycle)
            expected_sources = sources + (0,) * (16 - len(sources))
            if decoded != expected_sources:
                _fail(f"cycle {cycle} S2 source-lane mapping mismatch")
            saw_scattered |= sources != tuple(range(len(sources)))
            s2_fires += 1

        producer_fires += flags["producer_fire"]
        fifo_pushes += flags["fifo_push"]
        fifo_pops += flags["fifo_pop"]
        pe_inputs += flags["input_fire"]
        launches += flags["launch"]
        output_rows += flags["output_collected"]
        if flags["drained"] != (cycle + 1 == len(rows)):
            _fail("drained must be asserted only on the final trace cycle")

    expected_vectors = derived.im2col.expected_vectors
    expected_counts = {
        "s1_fires": s1_fires,
        "s2_fires": s2_fires,
        "producer_fires": producer_fires,
        "fifo_pushes": fifo_pushes,
        "fifo_pops": fifo_pops,
        "pe_inputs": pe_inputs,
    }
    for name, count in expected_counts.items():
        if count != expected_vectors:
            _fail(f"{name}={count}, expected {expected_vectors}")
    if launches != derived.expected_tiles:
        _fail(f"launches={launches}, expected {derived.expected_tiles}")
    expected_rows = (
        config.im2col.n * config.im2col.out_h * config.im2col.out_w)
    if output_rows != expected_rows:
        _fail(f"output rows={output_rows}, expected {expected_rows}")
    return {
        "cycles": len(rows),
        "saw_full_exchange": saw_full_exchange,
        "saw_input_bubble": saw_input_bubble,
        "saw_output_stall": saw_output_stall,
        "saw_scattered": saw_scattered,
    }


def verify_artifacts(
        fixture, trace, output, stats_path, reference_output=None,
        expect_conflicts=False, expect_scattered=False,
        expect_full_exchange=False, expect_input_bubbles=False,
        expect_output_backpressure=False):
    loaded = load_streaming_fixture(fixture)
    _read_output(output, loaded)
    if reference_output is not None:
        if Path(output).read_bytes() != Path(reference_output).read_bytes():
            _fail("streaming output differs from frozen current-model output")
    trace_result = _read_trace(trace, loaded)
    stats = _read_stats(stats_path)
    derived = loaded.derived
    expected_vectors = derived.im2col.expected_vectors
    integer_expectations = {
        "totalCycles": trace_result["cycles"],
        "drainedCycle": trace_result["cycles"] - 1,
        "pipelineInputVectors": expected_vectors,
        "pipelineOutputVectors": expected_vectors,
        "compactedSpatialVectors": expected_vectors,
        "fifoPushes": expected_vectors,
        "fifoPops": expected_vectors,
        "peLaunches": derived.expected_tiles,
        "peInputCycles": expected_vectors,
        "tilesGenerated": derived.expected_tiles,
        "tilesLaunched": derived.expected_tiles,
        "tilesCompleted": derived.expected_tiles,
        "outputRows": (
            loaded.config.im2col.n * loaded.config.im2col.out_h *
            loaded.config.im2col.out_w),
        "outputElements": derived.expected_outputs,
    }
    for name, expected in integer_expectations.items():
        actual = _stat(stats, name)
        if actual != expected:
            _fail(f"{name}={actual}, expected {expected}")

    conflicts = _stat(stats, "bankConflictVectors")
    extra_rounds = _stat(stats, "bankConflictExtraRounds")
    conflict_stalls = _stat(stats, "bankConflictStallCycles")
    if expect_conflicts and not (
            conflicts and extra_rounds and conflict_stalls):
        _fail("profile expected bank conflicts but conflict stats are zero")
    scattered = _stat(stats, "rawScatteredMaskVectors")
    tile_oracle = expected_tile_source_lanes(loaded.config)
    scattered_tiles = sum(
        sources != tuple(range(len(sources)))
        for sources, _coordinates in tile_oracle
    )
    expected_scattered = scattered_tiles * derived.k
    if scattered != expected_scattered:
        _fail(
            f"rawScatteredMaskVectors={scattered}, "
            f"independent oracle expected {expected_scattered}")
    if trace_result["saw_scattered"] != bool(expected_scattered):
        _fail("trace and independent oracle disagree on scattered mapping")
    if expect_scattered and not expected_scattered:
        _fail("profile expected scattered mappings but oracle found none")
    checks = (
        (expect_full_exchange, trace_result["saw_full_exchange"],
         "full FIFO simultaneous pop/push"),
        (expect_input_bubbles, trace_result["saw_input_bubble"],
         "PE input bubble"),
        (expect_output_backpressure, trace_result["saw_output_stall"],
         "output backpressure"),
    )
    for expected, observed, description in checks:
        if expected and not observed:
            _fail(f"profile did not observe expected {description}")
    if expect_input_bubbles and _stat(stats, "peInputBubbleCycles") == 0:
        _fail("trace observed no counted PE input bubbles")
    if expect_full_exchange and _stat(stats, "fifoPeakOccupancy") != 4:
        _fail("full-exchange profile did not reach FIFO depth four")
    pairs = _stat(stats, "conflictFreeOutputPairs")
    gaps = _stat(stats, "conflictFreeOutputGapCycles")
    maximum = _stat(stats, "conflictFreeOutputMaxGap")
    if pairs and (gaps != pairs or maximum != 1):
        _fail("qualified conflict-free output pairs do not establish II=1")
    return loaded


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fixture", required=True, type=Path)
    parser.add_argument("--trace", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--stats", required=True, type=Path)
    parser.add_argument("--reference-output", type=Path)
    parser.add_argument("--expect-conflicts", action="store_true")
    parser.add_argument("--expect-scattered", action="store_true")
    parser.add_argument("--expect-full-exchange", action="store_true")
    parser.add_argument("--expect-input-bubbles", action="store_true")
    parser.add_argument("--expect-output-backpressure", action="store_true")
    args = parser.parse_args(argv)
    try:
        loaded = verify_artifacts(
            args.fixture, args.trace, args.output, args.stats,
            reference_output=args.reference_output,
            expect_conflicts=args.expect_conflicts,
            expect_scattered=args.expect_scattered,
            expect_full_exchange=args.expect_full_exchange,
            expect_input_bubbles=args.expect_input_bubbles,
            expect_output_backpressure=args.expect_output_backpressure,
        )
    except (OSError, StreamingVerificationError, ValueError) as error:
        print(f"streaming verification failed: {error}")
        return 1
    print(
        "streaming verification passed: "
        f"fixture={loaded.config.name}, "
        f"vectors={loaded.derived.im2col.expected_vectors}, "
        f"tiles={loaded.derived.expected_tiles}, "
        f"outputs={loaded.derived.expected_outputs}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
