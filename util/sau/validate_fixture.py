#!/usr/bin/env python3
"""Validate the PLAN2 SAU RTL golden-fixture package contract."""

import argparse
import collections
import csv
import hashlib
import json
import sys
from pathlib import Path


REQUIRED_FILES = {
    "csr_writes.csv", "csr_snapshot.json", "architecture.csv",
    "diagnostic.csv", "manifest.json", "SHA256SUMS", "README.md",
}
EXPECTED_COVERAGE = {
    "int8_gemm_32x32x32_single_flow",
    "int8_gemm_32x256x256_m_sweep",
    "int8_gemm_64x32x256_k_sweep",
    "int8_gemm_64x256x32_n_sweep",
    "int8_gemm_64x256x256_baseline",
}
EXPECTED_HOLDOUT = {
    "int8_gemm_96x256x256_m_holdout",
    "int8_gemm_64x128x256_k_holdout",
    "int8_gemm_64x256x128_n_holdout",
}
DIAGNOSTIC_FIELDS = {
    "cycle", "command_id", "start", "core_state", "input_switch_s",
    "input_switch_f", "read_req", "read_response_valid", "data_a_valid",
    "data_b_valid", "result_valid", "write_req", "command_done",
}


def value_is_one(value):
    return value in (1, "1", "0x1", "0X1")


def validate_checksum(directory, errors):
    sums = directory / "SHA256SUMS"
    for line_number, line in enumerate(sums.read_text().splitlines(), start=1):
        if not line.strip():
            continue
        try:
            expected, name = line.split(maxsplit=1)
        except ValueError:
            errors.append(f"{directory}: SHA256SUMS line {line_number} malformed")
            continue
        artifact = directory / name.strip().lstrip("*")
        if not artifact.is_file():
            errors.append(f"{directory}: checksum artifact missing: {name}")
            continue
        actual = hashlib.sha256(artifact.read_bytes()).hexdigest()
        if actual != expected:
            errors.append(f"{directory}: checksum mismatch: {name}")


def validate_storage_trace(rows, manifest, label):
    """Check the fixed, externally visible SAU SRAM timing contract."""
    errors = []
    elaboration = manifest.get("elaboration_params", {})
    if "SRAM_DELAY" not in elaboration:
        return errors
    read_latency = int(elaboration["SRAM_DELAY"]) + 1
    requests_per_cycle = collections.Counter()
    accepted_reads = {}
    accepted_order = []
    response_order = []
    last_a_request = {}
    first_b_request = {}

    for row_number, row in enumerate(rows, start=2):
        event = row.get("event")
        if event not in {
                "read_accepted", "write_accepted",
                "read_response_visible"}:
            continue
        try:
            cycle = int(row["cycle"], 0)
            command_id = int(row["command_id"], 0)
            beat = int(row["beat"], 0)
        except (KeyError, TypeError, ValueError):
            errors.append(
                f"{label}: architecture row {row_number} has invalid "
                "storage event fields"
            )
            continue

        stream = row.get("stream")
        key = (command_id, stream, beat)
        if event in {"read_accepted", "write_accepted"}:
            requests_per_cycle[cycle] += 1
        if event == "read_accepted":
            if key in accepted_reads:
                errors.append(f"{label}: duplicate accepted read {key}")
                continue
            accepted_reads[key] = cycle
            accepted_order.append(key)
            if stream == "operand_a":
                last_a_request[command_id] = cycle
            elif stream == "operand_b":
                first_b_request.setdefault(command_id, cycle)
        elif event == "read_response_visible":
            response_order.append(key)
            if key not in accepted_reads:
                errors.append(f"{label}: response without accepted read {key}")
                continue
            actual_latency = cycle - accepted_reads[key]
            if actual_latency != read_latency:
                errors.append(
                    f"{label}: read {key} visible after {actual_latency} "
                    f"cycles, expected SRAM_DELAY + 1 = {read_latency}"
                )

    over_issued = sorted(
        cycle for cycle, count in requests_per_cycle.items() if count > 1
    )
    if over_issued:
        errors.append(
            f"{label}: shared SRAM port issued multiple requests at cycle "
            f"{over_issued[0]}"
        )
    if response_order != accepted_order:
        errors.append(f"{label}: SRAM read responses are missing or out of order")
    for command_id, first_b_cycle in first_b_request.items():
        if command_id not in last_a_request:
            errors.append(
                f"{label}: command {command_id} issued B without an A preload"
            )
        elif first_b_cycle <= last_a_request[command_id]:
            errors.append(
                f"{label}: command {command_id} issued B before completing A "
                "preload requests"
            )
    return errors


def validate_fixture(directory):
    errors = []
    missing = sorted(name for name in REQUIRED_FILES
                     if not (directory / name).is_file())
    if missing:
        return [f"{directory}: missing required files: {', '.join(missing)}"]

    try:
        manifest = json.loads((directory / "manifest.json").read_text())
        snapshots = json.loads((directory / "csr_snapshot.json").read_text())
    except json.JSONDecodeError as error:
        return [f"{directory}: invalid JSON: {error}"]

    required_manifest = {
        "fixture_name", "fixture_role", "precision", "operation",
        "matrix", "trans_mode", "reuse_mode", "command_count",
        "beat_bytes", "elaboration_params",
    }
    absent = sorted(required_manifest - manifest.keys())
    if absent:
        errors.append(f"{directory}: manifest lacks: {', '.join(absent)}")
    if manifest.get("fixture_name") != directory.name:
        errors.append(f"{directory}: manifest fixture_name does not match directory")
    if manifest.get("fixture_role") not in {"coverage", "holdout"}:
        errors.append(f"{directory}: invalid fixture_role")
    if manifest.get("precision") != "int8" or manifest.get("operation") != "GEMM":
        errors.append(f"{directory}: unsupported precision/operation")
    if not value_is_one(manifest.get("trans_mode")) or not value_is_one(
            manifest.get("reuse_mode")):
        errors.append(f"{directory}: manifest must use trans/reuse mode 0x1")
    elaboration = manifest.get("elaboration_params", {})
    for name in ("SA_SIZE", "ROW_NUM", "COL_NUM", "REGDEPTH",
                 "SRAM_DELAY", "ADDR_DELAY", "SRAM_DATA_WIDTH"):
        if name not in elaboration:
            errors.append(f"{directory}: elaboration parameter missing: {name}")
    if manifest.get("beat_bytes") != 32 or elaboration.get("SRAM_DATA_WIDTH") != 256:
        errors.append(f"{directory}: fixture must use 32-byte / 256-bit beats")

    if not isinstance(snapshots, list) or len(snapshots) != manifest.get("command_count"):
        errors.append(f"{directory}: snapshot count does not match command_count")
    else:
        for index, snapshot in enumerate(snapshots, start=1):
            if snapshot.get("command_id") != index:
                errors.append(f"{directory}: snapshot command IDs are not ordered")
            if not value_is_one(snapshot.get("trans_mode")) or not value_is_one(
                    snapshot.get("reuse_mode")):
                errors.append(f"{directory}: snapshot {index} has unsupported mode")
            if "start_write_cycle" not in snapshot or "start_write_row" not in snapshot:
                errors.append(f"{directory}: snapshot {index} lacks start mapping")

    with (directory / "architecture.csv").open(newline="") as trace:
        rows = list(csv.DictReader(trace))
    accepted = sum(row.get("event") == "command_accepted" for row in rows)
    complete = sum(row.get("event") == "command_complete" for row in rows)
    if accepted != manifest.get("command_count") or complete != accepted:
        errors.append(f"{directory}: architecture command closure mismatch")
    errors.extend(validate_storage_trace(rows, manifest, directory))

    with (directory / "diagnostic.csv").open(newline="") as diagnostic:
        fields = set(csv.DictReader(diagnostic).fieldnames or [])
    absent_diagnostic = sorted(DIAGNOSTIC_FIELDS - fields)
    if absent_diagnostic:
        errors.append(f"{directory}: diagnostic fields missing: " +
                      ", ".join(absent_diagnostic))
    validate_checksum(directory, errors)
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture_root", type=Path)
    args = parser.parse_args(argv)
    root = args.fixture_root
    if (root / "manifest.json").is_file():
        directories = [root]
    else:
        directories = []
        for path in sorted(candidate for candidate in root.iterdir()
                           if candidate.is_dir()):
            try:
                manifest = json.loads((path / "manifest.json").read_text())
            except (OSError, json.JSONDecodeError):
                continue
            if manifest.get("fixture_role") in {"coverage", "holdout"}:
                directories.append(path)
    errors = []
    roles = {"coverage": set(), "holdout": set()}
    for directory in directories:
        errors.extend(validate_fixture(directory))
        try:
            manifest = json.loads((directory / "manifest.json").read_text())
            if manifest.get("fixture_role") in roles:
                roles[manifest["fixture_role"]].add(directory.name)
        except (OSError, json.JSONDecodeError):
            pass
    if len(directories) > 1:
        if roles["coverage"] != EXPECTED_COVERAGE:
            errors.append("fixture root: coverage fixture inventory mismatch")
        if roles["holdout"] != EXPECTED_HOLDOUT:
            errors.append("fixture root: hold-out fixture inventory mismatch")
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
