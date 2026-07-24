#!/usr/bin/env python3
"""Build a PLAN2 SAU golden package from clock-sampled RTL signals."""

import argparse
import collections
import csv
import hashlib
import json
import subprocess
from pathlib import Path


SIGNAL_FIELDS = {
    "rst_n", "csr_we", "csr_write_type", "csr_addr", "csr_wdata",
    "csr_ready", "start", "core_state", "input_switch_s",
    "input_switch_f", "sram_enable", "sram_addr", "sram_wstrb",
    "read_response_valid", "read_response_last", "data_a_valid",
    "data_a_last", "data_b_valid", "data_b_last", "result_valid",
    "result_last", "internal_write_valid", "internal_write_last",
    "write_req", "command_done",
}

ARCH_FIELDS = (
    "cycle", "event", "command_id", "stream", "address", "beat", "phase",
)

DIAGNOSTIC_FIELDS = (
    "cycle", "command_id", "start", "core_state", "input_switch_s",
    "input_switch_f", "read_req", "read_addr", "read_response_valid",
    "read_response_last", "data_a_valid", "data_a_last", "data_b_valid",
    "data_b_last", "result_valid", "result_last", "internal_write_valid",
    "internal_write_last", "write_req", "write_addr", "command_done",
)


def bit(value, low, width):
    return (value >> low) & ((1 << width) - 1)


def as_bool(value):
    return value in {"1", "True", "true"}


def as_hex(value):
    if not value or any(char in value.lower() for char in "xz"):
        raise ValueError("invalid RTL hexadecimal value: {!r}".format(value))
    return int(value, 16)


def hex32(value):
    return "0x{:08x}".format(value & 0xFFFF_FFFF)


def hex64(value):
    return "0x{:016x}".format(value & 0xFFFF_FFFF_FFFF_FFFF)


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class CsrState:
    """Decode accepted csr.sv writes without inventing command fields."""

    def __init__(self):
        self.vertical_address = 0
        self.horizontal_address = 0
        self.output_address = 0
        self.bias_address = 0
        self.trans_mode = 0
        self.reuse_mode = 0
        self.register_mode = 0
        self.pe_work_mode = 0
        self.sa_flow_mode = 0
        self.conv_kernal = 0
        self.stride_flag = 0
        self.shift_flag = 0
        self.cutbit = 0
        self.flow_loop_times = 0
        self.input = {}
        self.vertical = {}
        self.register_input = {}
        self.output = {}

    def apply(self, address, data):
        index = (address >> 1) & 0x7
        if index == 0:
            self.horizontal_address = bit(data, 0, 32)
            self.vertical_address = bit(data, 32, 32)
        elif index == 1:
            self.register_input = {
                "x_burst": bit(data, 32, 6),
                "y_step": bit(data, 38, 8),
                "y_cycle": bit(data, 46, 6),
                "c_step": bit(data, 52, 8),
                "c_cycle": bit(data, 24, 8),
                "valid_y_start": bit(data, 0, 6),
                "valid_y_end": bit(data, 6, 6),
                "valid_x_start": bit(data, 12, 6),
                "valid_x_end": bit(data, 18, 6),
            }
        elif index == 2:
            self.input = {
                "x_step": bit(data, 0, 8),
                "x_burst": bit(data, 8, 6),
                "y_step": bit(data, 14, 8),
                "y_burst": bit(data, 22, 6),
                "flow_step": bit(data, 32, 8),
                "flow_burst": bit(data, 40, 6),
                "ins_step": bit(data, 46, 8),
                "ins_burst": bit(data, 54, 6),
            }
        elif index == 3:
            self.trans_mode = bit(data, 0, 2)
            self.register_mode = bit(data, 2, 2)
            self.reuse_mode = bit(data, 4, 2)
            self.pe_work_mode = bit(data, 6, 2)
            self.sa_flow_mode = bit(data, 8, 2)
            self.conv_kernal = bit(data, 10, 3)
            self.stride_flag = bit(data, 13, 1)
            self.shift_flag = bit(data, 14, 1)
            self.cutbit = bit(data, 15, 5)
            self.flow_loop_times = bit(data, 20, 6)
            self.bias_address = bit(data, 32, 32)
        elif index == 4:
            self.vertical = {
                "x_step": bit(data, 0, 8),
                "x_burst": bit(data, 8, 6),
                "y_step": bit(data, 14, 8),
                "y_cycle": bit(data, 22, 6),
                "flow_step": bit(data, 32, 8),
                "flow_cycle": bit(data, 40, 8),
                "ins_step": bit(data, 48, 8),
                "ins_cycle": bit(data, 56, 6),
            }
        elif index == 5:
            self.output = {
                "x_step": bit(data, 0, 8),
                "x_burst": bit(data, 32, 6),
                "y_step": bit(data, 8, 8),
                "y_burst": bit(data, 38, 6),
                "flow_step": bit(data, 16, 8),
                "flow_burst": bit(data, 44, 6),
                "ins_step": bit(data, 24, 8),
                "ins_burst": bit(data, 50, 6),
                "register_x_burst": 0,
                "register_y_step": 0,
                "register_y_cycle": 0,
                "register_c_step": 0,
                "register_c_cycle": bit(data, 56, 8),
            }
        elif index == 6:
            self.output.update({
                "register_x_burst": bit(data, 0, 6),
                "register_y_step": bit(data, 6, 8),
                "register_y_cycle": bit(data, 14, 6),
                "register_c_step": bit(data, 20, 8),
            })
            self.output_address = bit(data, 32, 32)

    def snapshot(self, command_id, cycle, row_number):
        return {
            "command_id": command_id,
            "start_write_cycle": cycle,
            "start_write_row": row_number,
            "trans_mode": hex(self.trans_mode),
            "reuse_mode": hex(self.reuse_mode),
            "vertical_address": hex32(self.vertical_address),
            "horizontal_address": hex32(self.horizontal_address),
            "output_address": hex32(self.output_address),
            "bias_address": hex32(self.bias_address),
            "flow_loop_times": self.flow_loop_times,
            "pe_work_mode": hex(self.pe_work_mode),
            "sa_flow_mode": hex(self.sa_flow_mode),
            "register_mode": hex(self.register_mode),
            "conv_kernal": hex(self.conv_kernal),
            "stride_flag": self.stride_flag,
            "shift_flag": self.shift_flag,
            "cutbit": self.cutbit,
            "input": dict(self.input),
            "vertical": dict(self.vertical),
            "register_input": dict(self.register_input),
            "output": dict(self.output),
        }


def read_sampled_rows(path, clock_period_ps):
    with path.open(newline="") as source:
        reader = csv.DictReader(source)
        if not reader.fieldnames:
            raise ValueError("{}: missing sampled CSV header".format(path))
        time_field = reader.fieldnames[0]
        missing = SIGNAL_FIELDS - set(reader.fieldnames)
        if missing:
            raise ValueError(
                "{}: missing sampled signals: {}".format(
                    path, ", ".join(sorted(missing))
                )
            )
        rows = list(reader)
    if not rows:
        raise ValueError("{}: no clock samples".format(path))

    first_time = int(rows[0][time_field])
    for index, row in enumerate(rows):
        time_ps = int(row[time_field])
        delta = time_ps - first_time
        if delta < 0 or delta % clock_period_ps:
            raise ValueError(
                "{}: sample {} is not on the {} ps clock grid".format(
                    path, index + 2, clock_period_ps
                )
            )
        row["cycle"] = delta // clock_period_ps
    return rows, first_time


def collect_csr(rows):
    writes = []
    snapshots = []
    state = CsrState()
    command_id = 0
    for row in rows:
        if not (as_bool(row["csr_we"]) and as_bool(row["csr_ready"])):
            continue
        address = as_hex(row["csr_addr"])
        if address >> 4 != 0x20:
            continue
        operation = as_hex(row["csr_write_type"])
        data = as_hex(row["csr_wdata"])
        writes.append({
            "cycle": row["cycle"],
            "csr_addr": "0x{:03x}".format(address),
            "csr_operation": operation,
            "csr_wdata": hex64(data),
            "accepted": 1,
        })
        state.apply(address, data)
        if as_bool(row["start"]):
            command_id += 1
            snapshots.append(
                state.snapshot(command_id, row["cycle"], len(writes))
            )
    if not writes or not snapshots:
        raise ValueError("sampled trace contains no accepted SAU commands")
    return writes, snapshots


def emit_event(events, cycle, event, command_id, stream, address, beat, phase):
    events.append({
        "cycle": cycle,
        "event": event,
        "command_id": command_id,
        "stream": stream,
        "address": address,
        "beat": beat,
        "phase": phase,
    })


def build_architecture(rows, snapshots):
    events = []
    current_id = 0
    phase = "idle"
    counters = collections.defaultdict(
        lambda: {
            "read_a": 0, "read_b": 0, "response_a": 0, "response_b": 0,
            "array_a": 0, "array_b": 0, "result": 0, "write": 0,
        }
    )
    read_queue = collections.deque()
    work_items = {
        snapshot["command_id"]:
            snapshot["input"]["x_burst"] *
            snapshot["input"]["y_burst"] *
            snapshot["input"]["ins_burst"] *
            snapshot["flow_loop_times"]
        for snapshot in snapshots
    }

    previous_row = None
    for row in rows:
        cycle = row["cycle"]
        if as_bool(row["start"]):
            current_id += 1
            phase = "operand_load"
            emit_event(
                events, cycle, "phase_changed", current_id, "none",
                "0x00000000", 0, phase
            )
            emit_event(
                events, cycle, "command_accepted", current_id, "none",
                "0x00000000", 0, phase
            )
        if current_id == 0:
            continue

        read_req = (
            as_bool(row["sram_enable"]) and
            as_hex(row["sram_wstrb"]) == 0
        )
        if read_req:
            if previous_row is None:
                raise ValueError("first sampled cycle contains an SRAM read")
            resident = as_bool(previous_row["resident_read_req"])
            streamed = as_bool(previous_row["stream_read_req"])
            if resident == streamed:
                raise ValueError(
                    "cycle {}: physical read has ambiguous registered source".
                    format(cycle)
                )
            stream = "operand_a" if resident else "operand_b"
            name = "read_a" if stream == "operand_a" else "read_b"
            beat = counters[current_id][name]
            counters[current_id][name] += 1
            read_queue.append((current_id, stream))
            emit_event(
                events, cycle, "read_accepted", current_id, stream,
                hex32(as_hex(row["sram_addr"])), beat, phase
            )

        if as_bool(row["read_response_valid"]):
            if not read_queue:
                raise ValueError(
                    "cycle {}: read response without accepted request".format(
                        cycle
                    )
                )
            response_id, stream = read_queue.popleft()
            name = "response_a" if stream == "operand_a" else "response_b"
            beat = counters[response_id][name]
            counters[response_id][name] += 1
            emit_event(
                events, cycle, "read_response_visible", response_id, stream,
                "0x00000000", beat, phase
            )

        has_array = as_bool(row["data_a_valid"]) or as_bool(row["data_b_valid"])
        final_b = (
            as_bool(row["data_b_valid"]) and
            counters[current_id]["array_b"] + 1 == work_items[current_id]
        )
        if phase == "operand_load" and has_array:
            phase = "array_active"
            emit_event(
                events, cycle, "phase_changed", current_id, "none",
                "0x00000000", 0, phase
            )
        if final_b:
            phase = "array_drain"
            emit_event(
                events, cycle, "phase_changed", current_id, "none",
                "0x00000000", 0, phase
            )
        for signal, stream, counter in (
                ("data_b_valid", "operand_b", "array_b"),
                ("data_a_valid", "operand_a", "array_a")):
            if as_bool(row[signal]):
                beat = counters[current_id][counter]
                counters[current_id][counter] += 1
                emit_event(
                    events, cycle, "array_input_accepted", current_id,
                    stream, "0x00000000", beat, phase
                )

        if as_bool(row["result_valid"]):
            beat = counters[current_id]["result"]
            counters[current_id]["result"] += 1
            emit_event(
                events, cycle, "result_produced", current_id, "output",
                "0x00000000", beat, phase
            )

        write_req = (
            as_bool(row["sram_enable"]) and
            as_hex(row["sram_wstrb"]) != 0
        )
        if write_req:
            if phase != "writeback":
                phase = "writeback"
                emit_event(
                    events, cycle, "phase_changed", current_id, "none",
                    "0x00000000", 0, phase
                )
            beat = counters[current_id]["write"]
            counters[current_id]["write"] += 1
            emit_event(
                events, cycle, "write_accepted", current_id, "output",
                hex32(as_hex(row["sram_addr"])), beat, phase
            )

        if as_bool(row["command_done"]):
            phase = "complete"
            emit_event(
                events, cycle, "phase_changed", current_id, "none",
                "0x00000000", 0, phase
            )
            emit_event(
                events, cycle, "command_complete", current_id, "none",
                "0x00000000", 0, phase
            )
        previous_row = row

    if read_queue:
        raise ValueError("sampled trace ended with pending read responses")
    if current_id != len(snapshots):
        raise ValueError("architecture command count differs from CSR snapshots")
    return events, counters


def build_diagnostic(rows):
    accepted_cycles = [
        row["cycle"] for row in rows
        if as_bool(row["csr_we"]) and as_bool(row["csr_ready"]) and
        as_hex(row["csr_addr"]) >> 4 == 0x20
    ]
    done_cycles = [
        row["cycle"] for row in rows if as_bool(row["command_done"])
    ]
    if not accepted_cycles or not done_cycles:
        raise ValueError("cannot identify diagnostic capture window")
    first_cycle = accepted_cycles[0]
    last_cycle = done_cycles[-1] + 2
    command_id = 0
    diagnostics = []
    for row in rows:
        cycle = row["cycle"]
        if cycle < first_cycle or cycle > last_cycle:
            continue
        if as_bool(row["start"]):
            command_id += 1
        read_req = (
            as_bool(row["sram_enable"]) and
            as_hex(row["sram_wstrb"]) == 0
        )
        write_req = (
            as_bool(row["sram_enable"]) and
            as_hex(row["sram_wstrb"]) != 0
        )
        diagnostics.append({
            "cycle": cycle,
            "command_id": command_id,
            "start": int(as_bool(row["start"])),
            "core_state": row["core_state"],
            "input_switch_s": "2'b" + row["input_switch_s"].zfill(2),
            "input_switch_f": "2'b" + row["input_switch_f"].zfill(2),
            "read_req": int(read_req),
            "read_addr": (
                hex32(as_hex(row["sram_addr"]))
                if read_req else "0x00000000"
            ),
            "read_response_valid": int(
                as_bool(row["read_response_valid"])
            ),
            "read_response_last": int(as_bool(row["read_response_last"])),
            "data_a_valid": int(as_bool(row["data_a_valid"])),
            "data_a_last": int(as_bool(row["data_a_last"])),
            "data_b_valid": int(as_bool(row["data_b_valid"])),
            "data_b_last": int(as_bool(row["data_b_last"])),
            "result_valid": int(as_bool(row["result_valid"])),
            "result_last": int(as_bool(row["result_last"])),
            "internal_write_valid": int(
                as_bool(row["internal_write_valid"])
            ),
            "internal_write_last": int(
                as_bool(row["internal_write_last"])
            ),
            "write_req": int(write_req),
            "write_addr": (
                hex32(as_hex(row["sram_addr"]))
                if write_req else "0x00000000"
            ),
            "command_done": int(as_bool(row["command_done"])),
        })
    return diagnostics


def write_csv(path, fieldnames, rows):
    with path.open("w", newline="") as output:
        writer = csv.DictWriter(
            output, fieldnames=fieldnames, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)


def git_text(root, *arguments):
    return subprocess.check_output(
        ["git", "-C", str(root)] + list(arguments)
    ).decode().strip()


def git_diff_hash(root):
    diff = subprocess.check_output(
        ["git", "-C", str(root), "diff", "--binary"]
    )
    return hashlib.sha256(diff).hexdigest()


def verify_simulation_log(path):
    text = path.read_text(errors="replace")
    if "*** TEST PASSED" not in text or "mismatches=0" not in text:
        raise ValueError(
            "{}: RTL simulation did not report pass with zero mismatches".
            format(path)
        )


def build_manifest(
        args, writes, snapshots, events, diagnostics, first_time):
    source_paths = {
        "fsdb": args.fsdb,
        "sim_log": args.sim_log,
        "sampled_csv": args.sampled_csv,
    }
    for name in (
            "instruction.hex", "memory.hex", "memory_mod_0.hex",
            "memory_mod_1.hex", "memory_mod_2.hex"):
        path = args.firmware_dir / name
        if path.is_file():
            source_paths["firmware/" + name] = path
    source_artifacts = {
        name: {
            "path": str(path.resolve()),
            "sha256": sha256_file(path),
        }
        for name, path in source_paths.items()
    }
    flow_values = {item["flow_loop_times"] for item in snapshots}
    cutbit_values = {item["cutbit"] for item in snapshots}
    if len(flow_values) != 1 or len(cutbit_values) != 1:
        raise ValueError("commands disagree on flow_loop_times or cutbit")
    return {
        "schema_version": 1,
        "fixture_name": args.fixture_name,
        "fixture_role": args.fixture_role,
        "sweep_dimension": args.sweep_dimension,
        "precision": "int8",
        "operation": "GEMM",
        "matrix": {"m": args.m, "k": args.k, "n": args.n},
        "trans_mode": "0x1",
        "reuse_mode": "0x1",
        "command_count": len(snapshots),
        "flow_loop_times": next(iter(flow_values)),
        "cutbit": next(iter(cutbit_values)),
        "sa_rows": 32,
        "sa_cols": 32,
        "sram_data_bits": 256,
        "beat_bytes": 32,
        "elaboration_params": {
            "SA_SIZE": 32,
            "ROW_NUM": 32,
            "COL_NUM": 32,
            "REGDEPTH": 256,
            "SRAM_DELAY": 3,
            "ADDR_DELAY": 2,
            "SRAM_DATA_WIDTH": 256,
            "clock_period_ns": args.clock_period_ps / 1000.0,
        },
        "reset_length_ns": 100,
        "trace_cycle_zero": {
            "definition": "first sampled SAU clock posedge in FSDB",
            "fsdb_time_ps": first_time,
        },
        "csr_acceptance": (
            "csr_we && csr_ready at a sampled SAU clock posedge; "
            "writes are ordered by clock then waveform column order"
        ),
        "command_start_cycles": [
            item["start_write_cycle"] for item in snapshots
        ],
        "command_start_rows": [
            item["start_write_row"] for item in snapshots
        ],
        "npu_repo_head": git_text(args.npu_root, "rev-parse", "HEAD"),
        "npu_worktree_diff_sha256": git_diff_hash(args.npu_root),
        "simulator": "VCS",
        "simulator_version": args.simulator_version,
        "run_command": args.run_command,
        "fsdb_path": str(args.fsdb.resolve()),
        "testcase_dir": str(args.firmware_dir.resolve()),
        "simulation_result": "TEST PASSED; zero matmul mismatches",
        "source_artifacts_sha256": source_artifacts,
        "architecture_row_count": len(events),
        "diagnostic_row_count": len(diagnostics),
        "csr_write_count": len(writes),
    }


def build_readme(args, manifest, events):
    counts = collections.Counter(row["event"] for row in events)
    starts = ", ".join(str(value) for value in manifest["command_start_cycles"])
    return """# {name} — Current RTL Golden Package

## Fixture

- Role: `{role}`
- Matrix: `{m} × {k} × {n}` (M × K × N)
- Precision/operation: INT8 GEMM
- Control path: `trans_mode=0x1`, `reuse_mode=0x1`
- Commands: {commands}
- `flow_loop_times`: {flows}
- `cutbit`: {cutbit}

## Provenance

- RTL head: `{head}`
- Simulator: `{simulator}`
- Run command: `{run_command}`
- Result: `TEST PASSED`, zero matmul mismatches
- FSDB: `{fsdb}`
- Sampled CSV: `{sampled}`

Cycle zero is the first sampled SAU clock positive edge in the FSDB
(`{first_time}` ps). CSR writes are accepted when `csr_we && csr_ready` are
both high at that sampled edge. Command start cycles: {starts}.

## Event counts

- `command_accepted`: {accepted}
- `read_accepted`: {reads}
- `read_response_visible`: {responses}
- `array_input_accepted`: {array_inputs}
- `result_produced`: {results}
- `write_accepted`: {writes}
- `command_complete`: {complete}

## Verification

```bash
sha256sum -c SHA256SUMS
```
""".format(
        name=args.fixture_name,
        role=args.fixture_role,
        m=args.m,
        k=args.k,
        n=args.n,
        commands=manifest["command_count"],
        flows=manifest["flow_loop_times"],
        cutbit=manifest["cutbit"],
        head=manifest["npu_repo_head"],
        simulator=manifest["simulator_version"],
        run_command=args.run_command,
        fsdb=args.fsdb.resolve(),
        sampled=args.sampled_csv.resolve(),
        first_time=manifest["trace_cycle_zero"]["fsdb_time_ps"],
        starts=starts,
        accepted=counts["command_accepted"],
        reads=counts["read_accepted"],
        responses=counts["read_response_visible"],
        array_inputs=counts["array_input_accepted"],
        results=counts["result_produced"],
        writes=counts["write_accepted"],
        complete=counts["command_complete"],
    )


def parse_args():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sampled-csv", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--fixture-name", required=True)
    parser.add_argument(
        "--fixture-role", choices=("coverage", "holdout"), required=True
    )
    parser.add_argument(
        "--sweep-dimension", choices=("m", "k", "n"), default=None
    )
    parser.add_argument("--m", type=int, required=True)
    parser.add_argument("--k", type=int, required=True)
    parser.add_argument("--n", type=int, required=True)
    parser.add_argument("--npu-root", type=Path, required=True)
    parser.add_argument("--sim-log", type=Path, required=True)
    parser.add_argument("--fsdb", type=Path, required=True)
    parser.add_argument("--firmware-dir", type=Path, required=True)
    parser.add_argument("--clock-period-ps", type=int, default=1667)
    parser.add_argument(
        "--simulator-version", default="VCS T-2022.06_Full64"
    )
    parser.add_argument("--run-command", required=True)
    return parser.parse_args()


def main():
    args = parse_args()
    if args.output_dir.exists() and any(args.output_dir.iterdir()):
        raise ValueError(
            "{}: refusing to overwrite nonempty package".format(
                args.output_dir
            )
        )
    args.output_dir.mkdir(parents=True, exist_ok=True)
    verify_simulation_log(args.sim_log)
    rows, first_time = read_sampled_rows(
        args.sampled_csv, args.clock_period_ps
    )
    writes, snapshots = collect_csr(rows)
    for snapshot in snapshots:
        if (
                snapshot["trans_mode"] != "0x1" or
                snapshot["reuse_mode"] != "0x1"):
            raise ValueError("unsupported RTL trans/reuse control path")
    events, _ = build_architecture(rows, snapshots)
    diagnostics = build_diagnostic(rows)

    write_csv(
        args.output_dir / "csr_writes.csv",
        ("cycle", "csr_addr", "csr_operation", "csr_wdata", "accepted"),
        writes,
    )
    (args.output_dir / "csr_snapshot.json").write_text(
        json.dumps(snapshots, indent=2) + "\n"
    )
    write_csv(args.output_dir / "architecture.csv", ARCH_FIELDS, events)
    write_csv(
        args.output_dir / "diagnostic.csv",
        DIAGNOSTIC_FIELDS,
        diagnostics,
    )
    manifest = build_manifest(
        args, writes, snapshots, events, diagnostics, first_time
    )
    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )
    (args.output_dir / "README.md").write_text(
        build_readme(args, manifest, events)
    )
    checksum_names = (
        "csr_writes.csv", "csr_snapshot.json", "architecture.csv",
        "diagnostic.csv", "manifest.json", "README.md",
    )
    with (args.output_dir / "SHA256SUMS").open("w") as sums:
        for name in checksum_names:
            sums.write(
                "{}  {}\n".format(
                    sha256_file(args.output_dir / name), name
                )
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
