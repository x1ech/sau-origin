#!/usr/bin/env python3
"""Run the frozen seven-profile pipeline RTL matrix."""

import argparse
import json
from pathlib import Path
import subprocess
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
DEFAULT_MATRIX = (
    REPOSITORY_ROOT / "tests/gem5/conv_pipeline/golden_matrix.json"
)


def run_matrix(
        matrix_path, sim_executable, outdir, simulator_name,
        simulator_version, timeout=None, dry_run=False):
    matrix_path = Path(matrix_path).resolve()
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    if matrix.get("schema_version") != 1:
        raise ValueError("pipeline matrix schema_version must be 1")
    profiles = matrix.get("profiles")
    if not isinstance(profiles, list) or len(profiles) != 7:
        raise ValueError("pipeline matrix must contain exactly seven profiles")
    output_root = Path(outdir).resolve()
    if output_root.exists():
        raise ValueError(f"refusing to overwrite matrix output: {output_root}")
    output_root.mkdir(parents=True, exist_ok=True)

    summary = []
    for profile in profiles:
        name = profile["name"]
        profile_dir = output_root / name
        profile_dir.mkdir(parents=True, exist_ok=True)
        fixture = REPOSITORY_ROOT / profile["fixture"]
        command = [
            sys.executable,
            str(REPOSITORY_ROOT /
                "util/conv_pipeline/rtl_pipeline_runner.py"),
            "--fixture", str(fixture),
            "--trace", str(profile_dir / "trace.csv"),
            "--output", str(profile_dir / "output.csv"),
            "--manifest", str(profile_dir / "manifest.json"),
            "--output-ready-period",
            str(profile["output_ready_period"]),
            "--output-ready-high-cycles",
            str(profile["output_ready_high_cycles"]),
            "--sim-executable", str(Path(sim_executable).resolve()),
            "--simulator-name", simulator_name,
            "--simulator-version", simulator_version,
        ]
        if timeout is not None:
            command.extend(("--timeout", str(timeout)))
        if dry_run:
            command.append("--dry-run")
        result = subprocess.run(
            command,
            cwd=REPOSITORY_ROOT,
            check=False,
            capture_output=True,
            text=True,
        )
        log_path = profile_dir / "runner.log"
        log_path.write_text(
            result.stdout + result.stderr,
            encoding="utf-8",
        )
        summary.append({
            "name": name,
            "fixture": profile["fixture"],
            "returncode": result.returncode,
            "runner_log": log_path.relative_to(output_root).as_posix(),
            "manifest": (
                profile_dir / "manifest.json"
            ).relative_to(output_root).as_posix(),
        })
        if result.returncode != 0:
            raise RuntimeError(
                f"pipeline profile {name} failed; see {log_path}")

    summary_path = output_root / "summary.json"
    summary_path.write_text(
        json.dumps(summary, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, default=DEFAULT_MATRIX)
    parser.add_argument("--sim-executable", type=Path, required=True)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--simulator-name", required=True)
    parser.add_argument("--simulator-version", required=True)
    parser.add_argument("--timeout", type=int)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        summary = run_matrix(
            args.matrix, args.sim_executable, args.outdir,
            args.simulator_name, args.simulator_version,
            args.timeout, args.dry_run)
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    print(f"PASS pipeline RTL matrix profiles={len(summary)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
