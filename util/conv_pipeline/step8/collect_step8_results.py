#!/usr/bin/env python3
"""Validate workstation artifacts and create the Step 8 result manifest."""

import argparse
import hashlib
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from util.conv_pipeline.pipeline_fixture import load_fixture
from util.conv_pipeline.rtl_pipeline_runner import (
    RTL_SOURCES,
    validate_rtl_result,
)
from util.conv_pipeline.array.verify_sau_array_trace import validate


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
MATRIX_PATH = REPOSITORY_ROOT / "tests/gem5/conv_pipeline/golden_matrix.json"


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def collect(outdir):
    root = Path(outdir).resolve()
    manifest_path = root / "result_manifest.json"
    if manifest_path.exists():
        raise FileExistsError(f"refusing to overwrite {manifest_path}")
    legacy_log = root / "legacy/run.log"
    if "PASS tb_gemmini_im2col_chw_gather_readable" not in (
            legacy_log.read_text(encoding="utf-8", errors="replace")):
        raise ValueError("legacy Im2Col regression did not report PASS")
    version_lines = (root / "vcs_version.txt").read_text(
        encoding="utf-8", errors="replace").splitlines()
    if not version_lines or not version_lines[0].strip():
        raise ValueError("VCS version output is empty")
    vcs_version = version_lines[0]

    artifacts = {
        "legacy/run.log": sha256_file(legacy_log),
        "legacy/compile.log": sha256_file(root / "legacy/compile.log"),
        "standalone/compile.log": sha256_file(
            root / "standalone/compile.log"),
        "pipeline/compile.log": sha256_file(root / "pipeline/compile.log"),
        "vcs_version.txt": sha256_file(root / "vcs_version.txt"),
    }
    standalone = []
    for case_name in (
            "tail_r1_c1_k9", "tail_r15_c15_k9", "full_r16_c16_k9",
            "backpressure_r3_c3_k9", "sat_pos_r1_c1_k567",
            "sat_neg_r1_c1_k567"):
        trace = root / "standalone" / f"{case_name}.csv"
        log = root / "standalone" / f"{case_name}.log"
        cycles, output_rows = validate(trace, case_name)
        for path in (trace, log):
            artifacts[path.relative_to(root).as_posix()] = sha256_file(path)
        standalone.append({
            "case": case_name,
            "cycles": cycles,
            "output_rows": output_rows,
            "trace": trace.relative_to(root).as_posix(),
            "trace_sha256": sha256_file(trace),
            "log": log.relative_to(root).as_posix(),
            "log_sha256": sha256_file(log),
        })

    matrix = json.loads(MATRIX_PATH.read_text(encoding="utf-8"))
    profiles = []
    for profile in matrix["profiles"]:
        name = profile["name"]
        profile_root = root / "pipeline/golden" / name
        fixture_path = REPOSITORY_ROOT / profile["fixture"]
        loaded = load_fixture(fixture_path)
        trace = profile_root / "trace.csv"
        output = profile_root / "output.csv"
        runner_log = profile_root / "runner.log"
        manifest = profile_root / "manifest.json"
        simulator_output = runner_log.read_text(
            encoding="utf-8", errors="replace")
        anchors = validate_rtl_result(
            loaded, trace, output, simulator_output)
        fixture_manifest = json.loads(manifest.read_text(encoding="utf-8"))
        if fixture_manifest.get("schema_version") != 1 or (
                fixture_manifest.get("status") !=
                "rtl_pipeline_fixture_passed"):
            raise ValueError(f"profile {name} manifest does not report PASS")
        if fixture_manifest.get("resolved_config_sha256") != (
                loaded.resolved_config_sha256):
            raise ValueError(f"profile {name} manifest config hash mismatch")
        if fixture_manifest.get("sources") != {
                relative: sha256_file(REPOSITORY_ROOT / relative)
                for relative in RTL_SOURCES}:
            raise ValueError(f"profile {name} manifest source mismatch")
        if fixture_manifest.get("runtime") != {
                "output_ready_period": profile["output_ready_period"],
                "output_ready_high_cycles":
                profile["output_ready_high_cycles"]}:
            raise ValueError(f"profile {name} manifest runtime mismatch")
        if fixture_manifest.get("trace_sha256") != sha256_file(trace) or (
                fixture_manifest.get("output_sha256") !=
                sha256_file(output)):
            raise ValueError(f"profile {name} manifest artifact hash mismatch")
        if fixture_manifest.get("anchors") != anchors:
            raise ValueError(f"profile {name} manifest anchor mismatch")
        fixture_simulator = fixture_manifest.get("simulator", {})
        if fixture_simulator.get("name") != "VCS" or (
                fixture_simulator.get("version") != vcs_version):
            raise ValueError(f"profile {name} simulator mismatch")
        for path in (trace, output, runner_log, manifest):
            artifacts[path.relative_to(root).as_posix()] = sha256_file(path)
        profiles.append({
            "name": name,
            "fixture": profile["fixture"],
            "resolved_config_sha256": loaded.resolved_config_sha256,
            "trace": trace.relative_to(root).as_posix(),
            "trace_sha256": sha256_file(trace),
            "output": output.relative_to(root).as_posix(),
            "output_sha256": sha256_file(output),
            "manifest": manifest.relative_to(root).as_posix(),
            "manifest_sha256": sha256_file(manifest),
            "anchors": anchors,
        })

    sources = {
        relative: sha256_file(REPOSITORY_ROOT / relative)
        for relative in RTL_SOURCES
    }
    result = {
        "schema_version": 1,
        "status": "step8_workstation_matrix_passed",
        "scope": (
            "local RTL/oracle validation; gem5 strict comparison pending"
        ),
        "golden_object": (
            "validated Im2Col plus project-owned sau_array_16x16 fusion"
        ),
        "sources": sources,
        "simulator": {
            "name": "VCS",
            "version": vcs_version,
        },
        "matrix_sha256": sha256_file(MATRIX_PATH),
        "profiles": profiles,
        "standalone": standalone,
        "artifacts": dict(sorted(artifacts.items())),
    }
    manifest_path.write_text(
        json.dumps(result, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest_path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outdir", type=Path, required=True)
    args = parser.parse_args()
    try:
        path = collect(args.outdir)
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"Step 8 result collection failed: {error}", file=sys.stderr)
        return 1
    print(f"PASS Step 8 result manifest {path} sha256={sha256_file(path)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
