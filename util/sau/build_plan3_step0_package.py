#!/usr/bin/env python3
"""Build a self-checking PLAN3 Step-0 RTL functional-reference package."""

import argparse
import csv
import hashlib
import json
import shutil
from pathlib import Path


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--name", required=True)
    parser.add_argument("--role", required=True)
    parser.add_argument("--memory", type=Path, required=True)
    parser.add_argument("--compare", type=Path, required=True)
    parser.add_argument("--boundary", type=Path, required=True)
    parser.add_argument("--sim-log", type=Path, required=True)
    parser.add_argument("--fsdb", type=Path, required=True)
    parser.add_argument("--trans", type=int, required=True)
    parser.add_argument("--reuse", type=int, required=True)
    parser.add_argument("--cutbit", type=int, required=True)
    parser.add_argument("--expected-mismatches", type=int, required=True)
    parser.add_argument("--worktree-diff-sha256", required=True)
    parser.add_argument("--csr-writes", type=Path)
    parser.add_argument("--csr-snapshot", type=Path)
    args = parser.parse_args()

    args.output.mkdir(parents=True, exist_ok=True)
    copied = {
        "initial_memory.hex": args.memory,
        "boundary.csv": args.boundary,
        "matmul_compare.csv": args.compare,
        "sim.log": args.sim_log,
    }
    if args.csr_writes:
        copied["csr_writes.csv"] = args.csr_writes
    if args.csr_snapshot:
        copied["csr_snapshot.json"] = args.csr_snapshot
    for name, source in copied.items():
        shutil.copy2(source, args.output / name)

    actual = []
    mismatches = 0
    with args.compare.open(newline="") as stream:
        for row in csv.DictReader(stream):
            actual.append(int(row["actual_hex"], 16))
            mismatches += row["match"] != "1"
    if mismatches != args.expected_mismatches:
        raise SystemExit(
            f"expected {args.expected_mismatches} mismatches, found {mismatches}"
        )
    (args.output / "final_output_memory.hex").write_text(
        "".join(f"{value:02x}\n" for value in actual)
    )

    manifest = {
        "schema_version": 1,
        "fixture_name": args.name,
        "fixture_role": args.role,
        "rtl_contract": {
            "npu_repo_head": "d894466f15ea84cffab5596fa87fc33767c78361",
            "npu_worktree_diff_sha256": args.worktree_diff_sha256,
            "simv_sha256":
                "c8048098eb5237ffe5e39bb54d917fd4fe9ef501757f30ef07433854e498df57",
            "fsdb_path": str(args.fsdb),
            "fsdb_sha256": sha256(args.fsdb),
            "simulator": "VCS T-2022.06_Full64",
        },
        "elaboration_params": {
            "SA_SIZE": 32,
            "ROW_NUM": 32,
            "COL_NUM": 32,
            "OUTPUTDW": 24,
            "SRAM_DATA_WIDTH": 256,
            "SRAM_DELAY": 3,
            "ADDR_DELAY": 2,
            "REGDEPTH": 256,
            "clock_period_ns": 1.667,
        },
        "operation": "int8_gemm",
        "matrix": {"m": 32, "k": 32, "n": 32},
        "csr_modes": {
            "trans_mode": args.trans,
            "reuse_mode": args.reuse,
            "sa_flow_mode": 0,
            "register_mode": 0,
            "pe_work_mode": 0,
            "conv_kernal": 0,
            "stride_flag": 0,
            "shift_flag": 0,
            "cutbit": args.cutbit,
        },
        "memory_contract": {
            "initial_image": "initial_memory.hex",
            "initial_image_semantics": "pre-simulation firmware data image",
            "output_base": "0x29120c00",
            "output_size_bytes": len(actual),
            "output_end_exclusive": f"0x{0x29120c00 + len(actual):08x}",
            "output_byte_order": "ascending byte address, one int8 byte per line",
            "external_beat_bytes": 32,
            "boundary_trace": "boundary.csv",
            "final_output": "final_output_memory.hex",
        },
        "result": {
            "element_count": len(actual),
            "software_reference_mismatches": mismatches,
            "software_reference_is_oracle": mismatches == 0,
        },
        "replay": {
            "firmware_build":
                "make all mod3 SAU_TEST_CASE=1",
            "simulation":
                "env SIM_TB_NAME=top_yinglong_tb FSDB_NAME=<name> "
                "<simv> -l sim.log -ucli +assert_enable +fsdb+autofsdb "
                "-i sim/vcs/script/case_yinglong/simv_verdi.tcl "
                "+firmware=<firmware-build-dir>",
            "waveform_reader":
                "/home/xch/work/npi_fsdb_probe/src/npi_fsdb_probe",
        },
    }
    (args.output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n"
    )

    files = sorted(
        path for path in args.output.iterdir()
        if path.is_file() and path.name != "SHA256SUMS"
    )
    (args.output / "SHA256SUMS").write_text(
        "".join(f"{sha256(path)}  {path.name}\n" for path in files)
    )


if __name__ == "__main__":
    main()
