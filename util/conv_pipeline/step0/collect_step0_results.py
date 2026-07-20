#!/usr/bin/env python3
"""Create the provenance manifest for a completed Step 0 VCS matrix."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))
from util.conv_pipeline.step0.verify_step0_trace import CASES, validate_trace


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def vcs_version(command: str) -> str:
    result = subprocess.run(
        [command, "-ID"],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    output = "\n".join(line.rstrip() for line in result.stdout.splitlines())
    if not output:
        raise RuntimeError(f"{command} -ID produced no version output")
    return output


def collect(root: Path, outdir: Path, vcs_command: str) -> dict[str, object]:
    artifacts: dict[str, str] = {}
    runs = []
    for variant in ("original", "integration"):
        compile_log = outdir / variant / "compile.log"
        if not compile_log.is_file():
            raise FileNotFoundError(compile_log)
        artifacts[compile_log.relative_to(outdir).as_posix()] = sha256(compile_log)
        for case_name in CASES:
            trace = outdir / variant / f"{case_name}.csv"
            log = outdir / variant / f"{case_name}.log"
            if not log.is_file():
                raise FileNotFoundError(log)
            cycles, output_rows = validate_trace(trace, case_name, variant)
            artifacts[trace.relative_to(outdir).as_posix()] = sha256(trace)
            artifacts[log.relative_to(outdir).as_posix()] = sha256(log)
            runs.append(
                {
                    "variant": variant,
                    "case": case_name,
                    "cycles": cycles,
                    "output_rows": output_rows,
                    "trace": trace.relative_to(outdir).as_posix(),
                    "trace_sha256": sha256(trace),
                    "log": log.relative_to(outdir).as_posix(),
                    "log_sha256": sha256(log),
                }
            )

    source_paths = {
        "mikui_provenance": root / "src/sau_n/rtl/mikui/provenance.json",
        "step0_testbench": root / "src/sau_n/rtl/tb_mikui_sau_engine_step0.sv",
        "source_verifier": root
        / "util/conv_pipeline/step0/verify_step0_sources.py",
        "trace_verifier": root
        / "util/conv_pipeline/step0/verify_step0_trace.py",
        "run_script": root / "util/conv_pipeline/step0/run_step0_vcs.sh",
    }

    return {
        "schema_version": 1,
        "status": "step0_vcs_matrix_passed",
        "upstream_commit": "2ca8252ef1cac43ef843998e9e08023259ac17ee",
        "golden_object": (
            "2ca8252 with documented FINISH_ROW/FINISH_COL width patch"
        ),
        "simulator": {
            "name": "VCS",
            "version_output": vcs_version(vcs_command),
            "host": platform.platform(),
        },
        "sources": {
            name: {
                "path": path.relative_to(root).as_posix(),
                "sha256": sha256(path),
            }
            for name, path in source_paths.items()
        },
        "runs": runs,
        "artifacts": dict(sorted(artifacts.items())),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=SCRIPT_ROOT)
    parser.add_argument("--outdir", type=Path, required=True)
    parser.add_argument("--vcs", default="vcs")
    args = parser.parse_args()

    root = args.root.resolve()
    outdir = args.outdir.resolve()
    manifest_path = outdir / "result_manifest.json"
    if manifest_path.exists():
        raise FileExistsError(f"refusing to overwrite {manifest_path}")
    manifest = collect(root, outdir, args.vcs)
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(f"PASS Step 0 result manifest {manifest_path} sha256={sha256(manifest_path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
