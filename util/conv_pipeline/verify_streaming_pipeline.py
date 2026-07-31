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
    "a_request_mask", "a_grant_mask", "a_response_mask",
    "a_request_tile", "a_request_k", "a_response_tile", "a_response_k",
    "b_request_mask", "b_grant_mask", "b_response_mask",
    "b_request_buffer", "b_request_slot", "b_request_k",
    "b_response_buffer", "b_response_slot", "b_response_k",
    "c_request_mask", "c_grant_mask", "c_response_mask",
    "c_request_byte", "c_response_byte",
    "d_queue_occupancy", "d_head_pending_mask",
    "d_request_mask", "d_grant_mask", "d_head_will_retire",
    "d_enqueue", "d_dequeue",
    "b_entry_hit", "b_reuse_hit", "active_b_buffer",
    "next_expected_k", "b_ready_entries",
)

BOOL_FIELDS = (
    "s0_valid", "s0_ready", "s0_fire", "s1_valid",
    "s1_can_retire", "s1_ready", "s1_fire", "s2_valid", "s2_ready",
    "s2_fire", "producer_fire", "producer_exhausted",
    "fifo_push_ready", "fifo_push", "fifo_pop", "fifo_head_valid",
    "pe_ready", "begin_launch", "launch", "input_valid", "input_fire",
    "sa_input_valid", "output_grant", "storage_ready",
    "row_score_valid", "cal_finish", "output_collected", "drained",
    "d_head_will_retire", "d_enqueue", "d_dequeue",
    "b_entry_hit", "b_reuse_hit",
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

def _mask(row, field, cycle):
    text = row[field]
    if not text.startswith("0x") or len(text) != 6:
        _fail(f"cycle {cycle} {field} has noncanonical width")
    value = _integer(text, f"cycle {cycle} {field}")
    if value > 0xffff:
        _fail(f"cycle {cycle} {field} exceeds 16 banks")
    return value


def expected_shared_grants(a_request, b_request, c_request, d_request):
    """Apply the frozen per-bank A > D > B policy."""
    if c_request:
        if a_request or b_request or d_request:
            _fail("C initialization must be exclusive")
        return 0, 0, c_request, 0
    a_grant = a_request
    d_grant = d_request & ~a_request & 0xffff
    b_grant = b_request & ~a_request & ~d_request & 0xffff
    return a_grant, b_grant, 0, d_grant


def expected_d_pending_decision(
        occupancy, depth, head_pending, write_grant, output_ready):
    """Replay depth-limited D retire/enqueue readiness from old state."""
    if depth <= 0 or occupancy < 0 or occupancy > depth:
        _fail("invalid D pending queue occupancy/depth")
    if occupancy == 0:
        if head_pending or write_grant:
            _fail("empty D pending queue cannot have a head or grants")
    else:
        if not head_pending:
            _fail("occupied D pending queue requires a pending head")
        if write_grant & ~head_pending:
            _fail("D write grant is not a subset of the pending head")
    retires = bool(occupancy) and not (head_pending & ~write_grant)
    push_ready = occupancy < depth or retires
    return retires, push_ready, bool(output_ready and push_ready)


class _BBufferReplay:
    """Independent B0/B1 state reconstructed from config and trace events."""

    def __init__(self, loaded):
        self.depth = loaded.config.shared_spad.b_buffer_depth
        self.k = loaded.derived.k
        self.expected_tiles = loaded.derived.expected_tiles
        self.output_mask = (1 << loaded.config.out_channels) - 1
        self.full_resident = (
            loaded.config.shared_spad.weight_reuse and
            2 * self.depth >= self.k)
        self.buffers = [None, None]
        self.active = 0
        self.next_k = 0
        self.next_chunk_base = 0
        self.fills = 0
        self.consumed = 0
        self.hits = 0
        self.empty_cycles = 0
        self.switches = 0
        self.reuse_hits = 0
        self._initialize()

    def _configure(self, index, base):
        length = min(self.depth, self.k - base) if base < self.k else 0
        self.buffers[index] = {
            "valid": length != 0,
            "base": base,
            "length": length,
            "entries": [
                {"k": base + slot, "ready": 0}
                for slot in range(length)
            ],
        }

    def _initialize(self):
        self._configure(0, 0)
        second_base = self.buffers[0]["length"]
        self._configure(1, second_base)
        self.next_chunk_base = (
            second_base + self.buffers[1]["length"])
        self.active = 0
        self.next_k = 0

    def _covers(self, index, k_index):
        buffer = self.buffers[index]
        return (
            buffer["valid"] and
            buffer["base"] <= k_index <
            buffer["base"] + buffer["length"])

    def _entry(self, index, k_index):
        if not self._covers(index, k_index):
            return None
        buffer = self.buffers[index]
        return buffer["entries"][k_index - buffer["base"]]

    def _complete(self, index):
        buffer = self.buffers[index]
        return (
            buffer["valid"] and
            all(entry["ready"] == self.output_mask
                for entry in buffer["entries"]))

    def _all_resident_ready(self):
        if not self.full_resident:
            return False
        next_k = 0
        for buffer in self.buffers:
            if not buffer["valid"]:
                continue
            if buffer["base"] != next_k:
                return False
            if not all(
                    entry["ready"] == self.output_mask
                    for entry in buffer["entries"]):
                return False
            next_k += buffer["length"]
        return next_k == self.k

    def ready_count(self):
        return sum(
            entry["ready"] == self.output_mask
            for buffer in self.buffers if buffer["valid"]
            for entry in buffer["entries"])

    def entry_ready(self, index, k_index):
        entry = self._entry(index, k_index)
        return entry is not None and entry["ready"] == self.output_mask

    def launch_ready(self):
        if self.full_resident:
            return self._all_resident_ready()
        return self.entry_ready(self.active, 0)

    def input_ready(self, k_index):
        return (
            k_index == self.next_k and
            self.entry_ready(self.active, self.next_k))

    def _switch(self, next_buffer):
        if (next_buffer == self.active or
                not self._complete(next_buffer) or
                not self._covers(next_buffer, self.next_k)):
            _fail("B replay encountered an invalid buffer switch")
        old_active = self.active
        self.active = next_buffer
        self.switches += 1
        if not self.full_resident:
            self._configure(old_active, self.next_chunk_base)
            self.next_chunk_base += self.buffers[old_active]["length"]

    def refresh(self):
        if self._covers(self.active, self.next_k):
            return
        other = 1 - self.active
        if self._complete(other) and self._covers(other, self.next_k):
            self._switch(other)

    def apply_response(self, mask, identity):
        if not mask:
            return
        buffer_index, slot, global_k = identity
        buffer = self.buffers[buffer_index]
        if (not buffer["valid"] or slot >= buffer["length"]):
            _fail("B response targets an invalid replay buffer slot")
        entry = buffer["entries"][slot]
        if entry["k"] != global_k:
            _fail("B response global K differs from replay entry")
        if entry["ready"] & mask:
            _fail("B response repeats an already-ready bank")
        was_complete = entry["ready"] == self.output_mask
        entry["ready"] |= mask
        if entry["ready"] & ~self.output_mask:
            _fail("B response exceeds configured output-channel mask")
        if not was_complete and entry["ready"] == self.output_mask:
            self.fills += 1

    def expected_request(self):
        for buffer_index in (self.active, 1 - self.active):
            buffer = self.buffers[buffer_index]
            if not buffer["valid"]:
                continue
            for slot, entry in enumerate(buffer["entries"]):
                if entry["ready"] == self.output_mask:
                    continue
                return (
                    self.output_mask & ~entry["ready"],
                    (buffer_index, slot, entry["k"]),
                )
        return 0, (0, 0, 0)

    def note_empty(self):
        self.empty_cycles += 1

    def consume(self, k_index, active_tile):
        if not self.input_ready(k_index):
            _fail("B replay consumed an entry that was not ready")
        self.consumed += 1
        self.hits += 1
        if self.full_resident and active_tile != 0:
            self.reuse_hits += 1
        self.next_k += 1
        if self.next_k == self.k:
            if active_tile + 1 < self.expected_tiles:
                self.next_k = 0
                if self.full_resident:
                    self.active = 0 if self._covers(0, 0) else 1
                else:
                    self._initialize()
            return
        if not self._covers(self.active, self.next_k):
            other = 1 - self.active
            if self._complete(other) and self._covers(other, self.next_k):
                self._switch(other)


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

def _float_stat(stats, name):
    if name not in stats:
        _fail(f"missing streamingPipeline.{name}")
    try:
        return float(stats[name])
    except ValueError as error:
        raise StreamingVerificationError(
            f"streamingPipeline.{name} must be numeric") from error


def _stat_vector(stats, name):
    result = []
    for bank in range(16):
        candidates = (
            f"{name}::{bank}",
            f"{name}[{bank}]",
        )
        key = next((candidate for candidate in candidates
                    if candidate in stats), None)
        if key is None:
            _fail(f"missing streamingPipeline.{name} bank {bank}")
        result.append(_integer(
            stats[key], f"streamingPipeline.{key}"))
    return tuple(result)


def _decode_source_lanes(text, cycle):
    if not text.startswith("0x") or len(text) != 34:
        _fail(f"cycle {cycle} s2_source_lanes has noncanonical width")
    value = _integer(text, f"cycle {cycle} s2_source_lanes")
    return tuple((value >> (row * 8)) & 0xff for row in range(16))


def _read_trace(
        path, loaded, output_ready_period=1,
        output_ready_high_cycles=1):
    if output_ready_period <= 0:
        _fail("output-ready period must be positive")
    if (output_ready_high_cycles <= 0 or
            output_ready_high_cycles > output_ready_period):
        _fail("output-ready high cycles must be in [1, period]")
    with Path(path).open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != TRACE_FIELDS:
            _fail("streaming trace does not use the 87-field schema v2")
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
    saw_full_ab_conflict = False
    saw_full_ab_two_slot_pair = False
    awaiting_full_b_retry = False
    spad_counts = {
        name: 0 for name in (
            "a_requests", "a_grants", "a_responses",
            "b_requests", "b_grants", "b_responses",
            "c_requests", "c_grants", "c_responses",
            "d_requests", "d_grants",
        )
    }
    per_bank_reads = [0] * 16
    per_bank_writes = [0] * 16
    per_bank_conflicts = [0] * 16
    previous_a_grant = 0
    previous_a_identity = None
    previous_b_grant = 0
    previous_b_identity = None
    previous_c_grant = 0
    previous_c_byte = None
    expected_d_occupancy = 0
    b_occupancy_sum = 0
    b_occupancy_peak = 0
    d_occupancy_peak = 0
    b_prefetch_stalls = 0
    d_write_stalls = 0
    pe_input_bubbles = 0
    output_channel_mask = (1 << config.out_channels) - 1
    b_replay = _BBufferReplay(loaded)
    bias_response_count = 0
    biases_ready = False

    for cycle, row in enumerate(rows):
        actual_cycle = _integer(row["cycle"], f"trace row {cycle} cycle")
        if actual_cycle != cycle:
            _fail(
                f"trace cycle discontinuity: expected {cycle}, "
                f"got {actual_cycle}")
        if row["schema_version"] != "2":
            _fail(f"cycle {cycle} has unexpected schema_version")
        if row["resolved_config_sha256"] != loaded.resolved_config_sha256:
            _fail(f"cycle {cycle} resolved config SHA256 mismatch")
        flags = {field: _boolean(row, field, cycle) for field in BOOL_FIELDS}
        masks = {
            field: _mask(row, field, cycle)
            for field in (
                "a_request_mask", "a_grant_mask", "a_response_mask",
                "b_request_mask", "b_grant_mask", "b_response_mask",
                "c_request_mask", "c_grant_mask", "c_response_mask",
                "d_head_pending_mask", "d_request_mask", "d_grant_mask",
            )
        }
        a_request = masks["a_request_mask"]
        a_grant = masks["a_grant_mask"]
        a_response = masks["a_response_mask"]
        b_request = masks["b_request_mask"]
        b_grant = masks["b_grant_mask"]
        b_response = masks["b_response_mask"]
        c_request = masks["c_request_mask"]
        c_grant = masks["c_grant_mask"]
        c_response = masks["c_response_mask"]
        d_pending = masks["d_head_pending_mask"]
        d_request = masks["d_request_mask"]
        d_grant = masks["d_grant_mask"]

        try:
            expected_grants = expected_shared_grants(
                a_request, b_request, c_request, d_request)
        except StreamingVerificationError as error:
            _fail(f"cycle {cycle} {error}")
        if (a_grant, b_grant, c_grant, d_grant) != expected_grants:
            _fail(f"cycle {cycle} grant masks violate A>D>B priority")
        if awaiting_full_b_retry:
            if (a_request != 0 or b_request != 0xffff or
                    a_grant != 0 or b_grant != 0xffff):
                _fail(
                    f"cycle {cycle} did not complete the full-bank B retry "
                    "after a full-bank A/B conflict")
            saw_full_ab_two_slot_pair = True
            awaiting_full_b_retry = False
        saw_full_ab_conflict |= (
            a_request == 0xffff and b_request == 0xffff and
            a_grant == 0xffff and b_grant == 0)
        if (a_request == 0xffff and b_request == 0xffff and
                a_grant == 0xffff and b_grant == 0):
            awaiting_full_b_retry = True
        b_prefetch_stalls += b_grant != b_request
        d_write_stalls += bool(d_request) and d_grant != d_request
        grant_masks = (a_grant, b_grant, c_grant, d_grant)
        for index, left in enumerate(grant_masks):
            for right in grant_masks[index + 1:]:
                if left & right:
                    _fail(f"cycle {cycle} bank granted multiple operations")
        if (a_response != previous_a_grant or
                b_response != previous_b_grant or
                c_response != previous_c_grant):
            _fail(f"cycle {cycle} read response is not the prior grant")
        if (b_request | b_response | c_request | c_response |
                d_request | d_grant) & ~output_channel_mask:
            _fail(f"cycle {cycle} B/C/D mask exceeds output channels")

        a_request_identity = (
            _integer(row["a_request_tile"], "a_request_tile"),
            _integer(row["a_request_k"], "a_request_k"),
        )
        a_response_identity = (
            _integer(row["a_response_tile"], "a_response_tile"),
            _integer(row["a_response_k"], "a_response_k"),
        )
        if a_request and (
                a_request_identity[0] >= derived.expected_tiles or
                a_request_identity[1] >= derived.k):
            _fail(f"cycle {cycle} A request identity exceeds bounds")
        if not a_request and a_request_identity != (0, 0):
            _fail(f"cycle {cycle} invalid A request is not canonical")
        if a_response and a_response_identity != previous_a_identity:
            _fail(f"cycle {cycle} A response identity mismatch")
        if not a_response and a_response_identity != (0, 0):
            _fail(f"cycle {cycle} invalid A response is not canonical")

        b_request_identity = (
            _integer(row["b_request_buffer"], "b_request_buffer"),
            _integer(row["b_request_slot"], "b_request_slot"),
            _integer(row["b_request_k"], "b_request_k"),
        )
        b_response_identity = (
            _integer(row["b_response_buffer"], "b_response_buffer"),
            _integer(row["b_response_slot"], "b_response_slot"),
            _integer(row["b_response_k"], "b_response_k"),
        )
        if b_request and (
                b_request_identity[0] >= 2 or
                b_request_identity[1] >=
                config.shared_spad.b_buffer_depth or
                b_request_identity[2] >= derived.k):
            _fail(f"cycle {cycle} B request identity exceeds bounds")
        if not b_request and b_request_identity != (0, 0, 0):
            _fail(f"cycle {cycle} invalid B request is not canonical")
        if b_response and b_response_identity != previous_b_identity:
            _fail(f"cycle {cycle} B response identity mismatch")
        if not b_response and b_response_identity != (0, 0, 0):
            _fail(f"cycle {cycle} invalid B response is not canonical")

        c_request_byte = _integer(
            row["c_request_byte"], "c_request_byte")
        c_response_byte = _integer(
            row["c_response_byte"], "c_response_byte")
        if c_request and c_request_byte not in (0, 1):
            _fail(f"cycle {cycle} C request byte exceeds int16")
        if not c_request and c_request_byte != 0:
            _fail(f"cycle {cycle} invalid C request is not canonical")
        if c_response and c_response_byte != previous_c_byte:
            _fail(f"cycle {cycle} C response byte mismatch")
        if not c_response and c_response_byte != 0:
            _fail(f"cycle {cycle} invalid C response is not canonical")

        replay_ready_entries = b_replay.ready_count()
        b_replay.refresh()
        replay_active_b = b_replay.active
        replay_next_k = b_replay.next_k
        consumer_state = _integer(
            row["consumer_state"], "consumer_state")
        fifo_count = _integer(
            row["fifo_count"], f"cycle {cycle} fifo_count")
        fifo_head_k = _integer(row["fifo_head_k"], "fifo_head_k")
        replay_input_ready = (
            biases_ready and fifo_count != 0 and
            consumer_state == 2 and
            b_replay.input_ready(fifo_head_k))
        replay_launch_ready = (
            biases_ready and fifo_count != 0 and
            consumer_state == 0 and fifo_head_k == 0 and
            b_replay.launch_ready())

        b_replay.apply_response(
            b_response, b_response_identity)
        expected_b_request, expected_b_identity = (
            b_replay.expected_request()
            if biases_ready else (0, (0, 0, 0)))
        if (b_request != expected_b_request or
                b_request_identity != expected_b_identity):
            _fail(
                f"cycle {cycle} B request/identity differs from "
                "independent B0/B1 replay")
        if flags["begin_launch"] != replay_launch_ready:
            _fail(
                f"cycle {cycle} launch readiness differs from B replay")
        if flags["input_fire"] != replay_input_ready:
            _fail(
                f"cycle {cycle} B readiness/input-fire differs from replay")
        if (fifo_count != 0 and consumer_state == 2 and
                not replay_input_ready):
            b_replay.note_empty()
        expected_reuse_hit = (
            flags["input_fire"] and b_replay.full_resident and
            _integer(row["active_tile"], "active_tile") != 0)

        d_occupancy = _integer(
            row["d_queue_occupancy"], "d_queue_occupancy")
        if d_occupancy != expected_d_occupancy:
            _fail(
                f"cycle {cycle} D occupancy={d_occupancy}, "
                f"expected {expected_d_occupancy}")
        if d_occupancy > config.shared_spad.d_pending_rows:
            _fail(f"cycle {cycle} D occupancy exceeds configured depth")
        if bool(d_occupancy) != bool(d_pending):
            _fail(f"cycle {cycle} D head mask/occupancy disagree")
        if d_pending & ~output_channel_mask:
            _fail(f"cycle {cycle} D pending mask exceeds output channels")
        if d_request != d_pending:
            _fail(f"cycle {cycle} D request does not equal old head mask")
        external_output_ready = (
            cycle % output_ready_period <
            output_ready_high_cycles)
        d_retires, d_push_ready, expected_output_grant = (
            expected_d_pending_decision(
                d_occupancy, config.shared_spad.d_pending_rows,
                d_pending, d_grant, external_output_ready))
        if flags["d_head_will_retire"] != d_retires:
            _fail(f"cycle {cycle} D head-retire decision mismatch")
        if flags["d_dequeue"] != d_retires:
            _fail(f"cycle {cycle} D dequeue does not match head retire")
        if flags["d_enqueue"] != flags["output_collected"]:
            _fail(f"cycle {cycle} D enqueue/output acceptance mismatch")
        if flags["d_enqueue"] and not flags["output_grant"]:
            _fail(f"cycle {cycle} D enqueue occurred without output grant")
        if flags["output_grant"] != expected_output_grant:
            _fail(
                f"cycle {cycle} output grant does not match periodic-ready "
                "and D queue readiness")
        expected_d_occupancy = (
            d_occupancy - int(d_retires) + int(flags["d_enqueue"]))
        d_occupancy_peak = max(d_occupancy_peak, expected_d_occupancy)

        active_b = _integer(row["active_b_buffer"], "active_b_buffer")
        next_expected_k = _integer(
            row["next_expected_k"], "next_expected_k")
        b_ready_entries = _integer(
            row["b_ready_entries"], "b_ready_entries")
        if active_b >= 2 or next_expected_k > derived.k:
            _fail(f"cycle {cycle} active B state exceeds bounds")
        if b_ready_entries > 2 * config.shared_spad.b_buffer_depth:
            _fail(f"cycle {cycle} B occupancy exceeds combined capacity")
        if (b_ready_entries != replay_ready_entries or
                active_b != replay_active_b or
                next_expected_k != replay_next_k):
            _fail(
                f"cycle {cycle} traced B state differs from independent "
                "B0/B1 replay")
        if flags["b_entry_hit"] != flags["input_fire"]:
            _fail(f"cycle {cycle} B hit/input fire mismatch")
        if flags["b_reuse_hit"] != expected_reuse_hit:
            _fail(f"cycle {cycle} B reuse hit differs from replay")
        b_occupancy_sum += b_ready_entries
        b_occupancy_peak = max(b_occupancy_peak, b_ready_entries)
        if flags["input_fire"]:
            b_replay.consume(
                fifo_head_k,
                _integer(row["active_tile"], "active_tile"))

        bias_response_count += c_response.bit_count()
        if bias_response_count > config.out_channels * 2:
            _fail("C response count exceeds two bytes per output channel")
        biases_ready = bias_response_count == config.out_channels * 2

        for prefix, value in (
                ("a_requests", a_request), ("a_grants", a_grant),
                ("a_responses", a_response), ("b_requests", b_request),
                ("b_grants", b_grant), ("b_responses", b_response),
                ("c_requests", c_request), ("c_grants", c_grant),
                ("c_responses", c_response), ("d_requests", d_request),
                ("d_grants", d_grant)):
            spad_counts[prefix] += value.bit_count()
        read_grant = a_grant | b_grant | c_grant
        competing_read = a_request | b_request | c_request
        for bank in range(16):
            bit = 1 << bank
            per_bank_reads[bank] += bool(read_grant & bit)
            per_bank_writes[bank] += bool(d_grant & bit)
            per_bank_conflicts[bank] += bool(
                d_request & competing_read & bit)

        previous_a_grant = a_grant
        previous_a_identity = a_request_identity if a_grant else None
        previous_b_grant = b_grant
        previous_b_identity = b_request_identity if b_grant else None
        previous_c_grant = c_grant
        previous_c_byte = c_request_byte if c_grant else None

        if fifo_count > 4:
            _fail(f"cycle {cycle} FIFO count exceeds depth four")
        saw_full_exchange |= (
            fifo_count == 4 and flags["fifo_push"] and flags["fifo_pop"])
        input_bubble = (
            _integer(row["consumer_state"], "consumer_state") == 2 and
            _integer(row["accepted_k"], "accepted_k") < derived.k and
            not flags["input_fire"]
        )
        saw_input_bubble |= input_bubble
        pe_input_bubbles += input_bubble
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

    if expected_d_occupancy != 0:
        _fail("D queue is not empty after final trace cycle")
    if awaiting_full_b_retry:
        _fail("trace ended before a full-bank B retry completed")
    if previous_a_grant or previous_b_grant or previous_c_grant:
        _fail("trace ended with an outstanding scratchpad read")
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
        "saw_full_ab_conflict": saw_full_ab_conflict,
        "saw_full_ab_two_slot_pair": saw_full_ab_two_slot_pair,
        "spad_counts": spad_counts,
        "per_bank_reads": tuple(per_bank_reads),
        "per_bank_writes": tuple(per_bank_writes),
        "per_bank_conflicts": tuple(per_bank_conflicts),
        "b_occupancy_sum": b_occupancy_sum,
        "b_occupancy_peak": b_occupancy_peak,
        "d_occupancy_peak": d_occupancy_peak,
        "b_prefetch_stalls": b_prefetch_stalls,
        "d_write_stalls": d_write_stalls,
        "b_replay_fills": b_replay.fills,
        "b_replay_consumed": b_replay.consumed,
        "b_replay_hits": b_replay.hits,
        "b_replay_empty_cycles": b_replay.empty_cycles,
        "b_replay_switches": b_replay.switches,
        "b_replay_reuse_hits": b_replay.reuse_hits,
        "pe_input_bubbles": pe_input_bubbles,
    }


def verify_artifacts(
        fixture, trace, output, stats_path, reference_output=None,
        expect_conflicts=False, expect_scattered=False,
        expect_full_exchange=False, expect_input_bubbles=False,
        expect_output_backpressure=False, expect_b_refill=False,
        expect_weight_reuse=False, expect_depth_one_baseline=False,
        expect_full_ab_conflict=False, output_ready_period=1,
        output_ready_high_cycles=1):
    loaded = load_streaming_fixture(fixture)
    _read_output(output, loaded)
    if reference_output is not None:
        if Path(output).read_bytes() != Path(reference_output).read_bytes():
            _fail("streaming output differs from frozen current-model output")
    trace_result = _read_trace(
        trace, loaded, output_ready_period,
        output_ready_high_cycles)
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

    trace_counts = trace_result["spad_counts"]
    spad_stat_names = {
        "a_requests": "spadReadRequestsA",
        "a_grants": "spadReadGrantsA",
        "a_responses": "spadReadResponsesA",
        "b_requests": "spadReadRequestsB",
        "b_grants": "spadReadGrantsB",
        "b_responses": "spadReadResponsesB",
        "c_requests": "spadReadRequestsC",
        "c_grants": "spadReadGrantsC",
        "c_responses": "spadReadResponsesC",
        "d_requests": "spadWriteRequestsD",
        "d_grants": "spadWriteGrantsD",
    }
    for trace_name, stat_name in spad_stat_names.items():
        actual = _stat(stats, stat_name)
        expected = trace_counts[trace_name]
        if actual != expected:
            _fail(f"{stat_name}={actual}, trace expected {expected}")
    vector_expectations = {
        "perBankReadCycles": trace_result["per_bank_reads"],
        "perBankWriteCycles": trace_result["per_bank_writes"],
        "perBankReadWriteConflicts":
            trace_result["per_bank_conflicts"],
    }
    for name, expected in vector_expectations.items():
        actual = _stat_vector(stats, name)
        if actual != expected:
            _fail(f"{name}={actual}, trace expected {expected}")
    if (_stat(stats, "bBufferPeakOccupancy") !=
            trace_result["b_occupancy_peak"]):
        _fail("B buffer peak occupancy differs from trace")
    expected_b_average = (
        trace_result["b_occupancy_sum"] / trace_result["cycles"])
    if abs(
            _float_stat(stats, "bBufferAverageOccupancy") -
            expected_b_average) > 5e-6:
        _fail("B buffer average occupancy differs from trace")
    if (_stat(stats, "bPrefetchStallCycles") !=
            trace_result["b_prefetch_stalls"]):
        _fail("B prefetch stall cycles differ from trace")
    if (_stat(stats, "dPendingPeak") !=
            trace_result["d_occupancy_peak"]):
        _fail("D pending peak differs from trace")
    if (_stat(stats, "dWriteStallCycles") !=
            trace_result["d_write_stalls"]):
        _fail("D write stall cycles differ from trace")
    if (_stat(stats, "peInputBubbleCycles") !=
            trace_result["pe_input_bubbles"]):
        _fail("PE input bubble cycles differ from trace")

    expected_c_reads = loaded.config.out_channels * 2
    for name in (
            "spadReadRequestsC", "spadReadGrantsC",
            "spadReadResponsesC"):
        actual = _stat(stats, name)
        if actual != expected_c_reads:
            _fail(f"{name}={actual}, expected {expected_c_reads}")

    d_requests = _stat(stats, "spadWriteRequestsD")
    d_grants = _stat(stats, "spadWriteGrantsD")
    if d_grants != derived.expected_outputs:
        _fail(
            f"spadWriteGrantsD={d_grants}, "
            f"expected {derived.expected_outputs}")
    if d_requests < d_grants:
        _fail(
            "D write conservation violated: "
            f"requests={d_requests}, grants={d_grants}")
    d_peak = _stat(stats, "dPendingPeak")
    if not 1 <= d_peak <= loaded.config.shared_spad.d_pending_rows:
        _fail(
            f"dPendingPeak={d_peak}, expected in [1, "
            f"{loaded.config.shared_spad.d_pending_rows}]")
    b_requests = _stat(stats, "spadReadRequestsB")
    b_grants = _stat(stats, "spadReadGrantsB")
    b_responses = _stat(stats, "spadReadResponsesB")
    b_fills = _stat(stats, "bBufferFillVectors")
    b_consumed = _stat(stats, "bBufferConsumedVectors")
    b_hits = _stat(stats, "bBufferHitVectors")
    b_switches = _stat(stats, "bBufferSwitches")
    b_empty_cycles = _stat(stats, "bBufferEmptyCycles")
    b_prefetch_stalls = _stat(stats, "bPrefetchStallCycles")
    reuse_hits = _stat(stats, "weightReuseHits")
    replay_b_stats = {
        "bBufferFillVectors": trace_result["b_replay_fills"],
        "bBufferConsumedVectors": trace_result["b_replay_consumed"],
        "bBufferHitVectors": trace_result["b_replay_hits"],
        "bBufferEmptyCycles": trace_result["b_replay_empty_cycles"],
        "bBufferSwitches": trace_result["b_replay_switches"],
        "weightReuseHits": trace_result["b_replay_reuse_hits"],
    }
    for name, expected in replay_b_stats.items():
        actual = _stat(stats, name)
        if actual != expected:
            _fail(
                f"{name}={actual}, independent B replay expected "
                f"{expected}")
    if not b_responses <= b_grants <= b_requests:
        _fail(
            "B read conservation violated: "
            f"responses={b_responses}, grants={b_grants}, "
            f"requests={b_requests}")
    if b_consumed != expected_vectors or b_hits != expected_vectors:
        _fail(
            "B consumption differs from expected vectors: "
            f"consumed={b_consumed}, hits={b_hits}, "
            f"expected={expected_vectors}")
    expected_b_responses = b_fills * loaded.config.out_channels
    if b_responses != expected_b_responses:
        _fail(
            f"spadReadResponsesB={b_responses}, "
            f"expected bBufferFillVectors*out_channels="
            f"{expected_b_responses}")

    shared = loaded.config.shared_spad
    fully_resident = (
        shared.weight_reuse and
        2 * shared.b_buffer_depth >= derived.k
    )
    expected_b_fills = derived.k if fully_resident else expected_vectors
    expected_reuse_hits = (
        (derived.expected_tiles - 1) * derived.k
        if fully_resident else 0
    )
    if b_fills != expected_b_fills:
        _fail(
            f"bBufferFillVectors={b_fills}, expected {expected_b_fills}")
    if reuse_hits != expected_reuse_hits:
        _fail(
            f"weightReuseHits={reuse_hits}, "
            f"expected {expected_reuse_hits}")
    if expect_b_refill:
        if fully_resident:
            _fail("B-refill profile unexpectedly has full K residency")
        if not (b_switches and b_empty_cycles and b_prefetch_stalls):
            _fail(
                "B-refill profile expected buffer switches, empty cycles, "
                "and prefetch stalls")
    if expect_weight_reuse:
        if not fully_resident:
            _fail("weight-reuse profile cannot hold the complete K range")
        if derived.expected_tiles > 1 and reuse_hits == 0:
            _fail("weight-reuse profile observed no cross-spatial reuse")
    if expect_depth_one_baseline:
        if shared.b_buffer_depth != 1 or shared.weight_reuse:
            _fail(
                "depth-one baseline requires b_buffer_depth=1 and "
                "weight_reuse=false")
        if b_fills != expected_vectors or reuse_hits != 0:
            _fail(
                "depth-one baseline must refill every K for every tile "
                "without reuse")
        if not (b_switches and b_empty_cycles and b_prefetch_stalls):
            _fail(
                "depth-one baseline expected buffer turnover, empty cycles, "
                "and prefetch stalls")
    if (expect_full_ab_conflict and
            not (trace_result["saw_full_ab_conflict"] and
                 trace_result["saw_full_ab_two_slot_pair"])):
        _fail(
            "profile never presented simultaneous full-bank A/B requests "
            "followed by a full-bank B retry in the next access slot")

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
    parser.add_argument("--output-ready-period", type=int, default=1)
    parser.add_argument(
        "--output-ready-high-cycles", type=int, default=1)
    parser.add_argument("--expect-conflicts", action="store_true")
    parser.add_argument("--expect-scattered", action="store_true")
    parser.add_argument("--expect-full-exchange", action="store_true")
    parser.add_argument("--expect-input-bubbles", action="store_true")
    parser.add_argument("--expect-output-backpressure", action="store_true")
    parser.add_argument("--expect-b-refill", action="store_true")
    parser.add_argument("--expect-weight-reuse", action="store_true")
    parser.add_argument("--expect-depth-one-baseline", action="store_true")
    parser.add_argument("--expect-full-ab-conflict", action="store_true")
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
            expect_b_refill=args.expect_b_refill,
            expect_weight_reuse=args.expect_weight_reuse,
            expect_depth_one_baseline=args.expect_depth_one_baseline,
            expect_full_ab_conflict=args.expect_full_ab_conflict,
            output_ready_period=args.output_ready_period,
            output_ready_high_cycles=args.output_ready_high_cycles,
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
