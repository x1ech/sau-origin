#!/usr/bin/env python3
"""Validate returned Step 8 RTL artifacts and optional gem5 traces."""

import argparse
import hashlib
import json
from pathlib import Path
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from util.conv_pipeline.compare_pipeline_traces import compare_trace_files
from util.conv_pipeline.pipeline_fixture import load_fixture
from util.conv_pipeline.rtl_pipeline_runner import (
    RTL_SOURCES,
    validate_output_file,
    validate_rtl_result,
)
from util.conv_pipeline.array.verify_sau_array_trace import validate


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
MATRIX_PATH = REPOSITORY_ROOT / "tests/gem5/conv_pipeline/golden_matrix.json"
ARRAY_VERIFIER_PATH = "util/conv_pipeline/array/verify_sau_array_trace.py"


class Step8ResultError(RuntimeError):
    """Raised when returned artifacts fail provenance or content checks."""


def sha256_file(path):
    digest = hashlib.sha256()
    try:
        with Path(path).open("rb") as source:
            for chunk in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as error:
        raise Step8ResultError(f"cannot hash {path}: {error}") from error
    return digest.hexdigest()


def _require(condition, message):
    if not condition:
        raise Step8ResultError(message)


def _source_manifest_matches(returned, expected):
    """Accept the historical verifier's single extra final LF only."""
    if not isinstance(returned, dict) or returned.keys() != expected.keys():
        return False
    for relative, expected_hash in expected.items():
        if returned[relative] == expected_hash:
            continue
        if relative != ARRAY_VERIFIER_PATH:
            return False
        current = (REPOSITORY_ROOT / relative).read_bytes()
        historical_hash = hashlib.sha256(current + b"\n").hexdigest()
        if returned[relative] != historical_hash:
            return False
    return True


def _artifact_path(root, relative):
    _require(isinstance(relative, str) and relative,
             "artifact path must be a non-empty string")
    candidate = (root / relative).resolve()
    try:
        candidate.relative_to(root)
    except ValueError as error:
        raise Step8ResultError(
            f"artifact path escapes result directory: {relative}") from error
    return candidate


def validate_results(results, gem5_results=None):
    root = Path(results).resolve()
    manifest_path = root / "result_manifest.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise Step8ResultError(
            f"cannot load result manifest {manifest_path}: {error}") \
            from error
    _require(manifest.get("schema_version") == 1,
             "result manifest schema_version must be 1")
    _require(manifest.get("status") == "step8_workstation_matrix_passed",
             "result manifest does not report a passed workstation matrix")
    _require(manifest.get("matrix_sha256") == sha256_file(MATRIX_PATH),
             "returned matrix hash differs from the current frozen matrix")
    expected_sources = {
        relative: sha256_file(REPOSITORY_ROOT / relative)
        for relative in RTL_SOURCES
    }
    _require(_source_manifest_matches(
                 manifest.get("sources"), expected_sources),
             "returned RTL source hashes differ from the current tree")
    simulator = manifest.get("simulator", {})
    _require(simulator.get("name") == "VCS",
             "returned top-level simulator must be VCS")
    _require(isinstance(simulator.get("version"), str) and
             simulator["version"].strip(),
             "returned top-level simulator version is missing")
    artifacts = manifest.get("artifacts")
    _require(isinstance(artifacts, dict),
             "returned artifact hash map is missing")
    matrix = json.loads(MATRIX_PATH.read_text(encoding="utf-8"))
    expected_artifacts = {
        "legacy/run.log", "legacy/compile.log", "standalone/compile.log",
        "pipeline/compile.log", "vcs_version.txt",
    }
    standalone_cases = {
        "tail_r1_c1_k9", "tail_r15_c15_k9", "full_r16_c16_k9",
        "backpressure_r3_c3_k9", "sat_pos_r1_c1_k567",
        "sat_neg_r1_c1_k567",
    }
    for case_name in standalone_cases:
        expected_artifacts.update({
            f"standalone/{case_name}.csv",
            f"standalone/{case_name}.log",
        })
    for profile in matrix["profiles"]:
        prefix = f"pipeline/golden/{profile['name']}"
        expected_artifacts.update({
            f"{prefix}/trace.csv", f"{prefix}/output.csv",
            f"{prefix}/runner.log", f"{prefix}/manifest.json",
        })
    _require(set(artifacts) == expected_artifacts,
             "returned artifact hash map is incomplete or unexpected")
    for relative, expected_hash in artifacts.items():
        _require(sha256_file(_artifact_path(root, relative)) == expected_hash,
                 f"returned artifact hash mismatch: {relative}")

    expected_profiles = {
        profile["name"]: profile for profile in matrix["profiles"]
    }
    returned_profiles = {
        profile["name"]: profile for profile in manifest.get("profiles", [])
    }
    _require(returned_profiles.keys() == expected_profiles.keys(),
             "returned profile names differ from the frozen matrix")
    strict_cycles = {}
    for name, profile in expected_profiles.items():
        returned = returned_profiles[name]
        expected_prefix = f"pipeline/golden/{name}"
        _require(
            returned.get("trace") == f"{expected_prefix}/trace.csv" and
            returned.get("output") == f"{expected_prefix}/output.csv" and
            returned.get("manifest") == f"{expected_prefix}/manifest.json",
            f"profile {name} artifact paths differ from the frozen layout",
        )
        loaded = load_fixture(REPOSITORY_ROOT / profile["fixture"])
        _require(returned["resolved_config_sha256"] ==
                 loaded.resolved_config_sha256,
                 f"profile {name} resolved config hash mismatch")
        trace = _artifact_path(root, returned["trace"])
        output = _artifact_path(root, returned["output"])
        fixture_manifest_path = _artifact_path(root, returned["manifest"])
        runner_log = trace.parent / "runner.log"
        _require(sha256_file(trace) == returned["trace_sha256"],
                 f"profile {name} trace hash mismatch")
        _require(sha256_file(output) == returned["output_sha256"],
                 f"profile {name} output hash mismatch")
        _require(sha256_file(fixture_manifest_path) ==
                 returned["manifest_sha256"],
                 f"profile {name} manifest hash mismatch")
        fixture_manifest = json.loads(
            fixture_manifest_path.read_text(encoding="utf-8"))
        _require(fixture_manifest.get("schema_version") == 1 and
                 fixture_manifest.get("status") ==
                 "rtl_pipeline_fixture_passed",
                 f"profile {name} manifest does not report PASS")
        _require(fixture_manifest.get("resolved_config_sha256") ==
                 loaded.resolved_config_sha256,
                 f"profile {name} fixture config hash mismatch")
        _require(_source_manifest_matches(
                     fixture_manifest.get("sources"), expected_sources),
                 f"profile {name} source provenance mismatch")
        _require(fixture_manifest.get("runtime") == {
                     "output_ready_period": profile["output_ready_period"],
                     "output_ready_high_cycles":
                     profile["output_ready_high_cycles"],
                 }, f"profile {name} runtime provenance mismatch")
        _require(fixture_manifest.get("trace_sha256") == sha256_file(trace),
                 f"profile {name} fixture trace hash mismatch")
        _require(fixture_manifest.get("output_sha256") == sha256_file(output),
                 f"profile {name} fixture output hash mismatch")
        _require(fixture_manifest.get("simulator", {}).get("name") == "VCS",
                 f"profile {name} simulator must be VCS")
        _require(
            fixture_manifest.get("simulator", {}).get("version") ==
            simulator["version"],
            f"profile {name} simulator version differs from top level",
        )
        anchors = validate_rtl_result(
            loaded, trace, output,
            runner_log.read_text(encoding="utf-8", errors="replace"))
        _require(fixture_manifest.get("anchors") == anchors and
                 returned.get("anchors") == anchors,
                 f"profile {name} anchor provenance mismatch")
        if gem5_results is not None:
            gem5_root = Path(gem5_results).resolve() / name
            gem5_trace = gem5_root / "trace.csv"
            gem5_output = gem5_root / "output.csv"
            validate_output_file(loaded, gem5_output)
            strict_cycles[name] = compare_trace_files(trace, gem5_trace)

    standalone = {
        item["case"]: item for item in manifest.get("standalone", [])
    }
    _require(set(standalone) == standalone_cases,
             "returned standalone matrix differs from six project-array cases")
    for case_name, item in standalone.items():
        _require(
            item.get("trace") == f"standalone/{case_name}.csv" and
            item.get("log") == f"standalone/{case_name}.log",
            f"standalone {case_name} paths differ from the frozen layout",
        )
        trace = _artifact_path(root, item["trace"])
        _require(sha256_file(trace) == item["trace_sha256"],
                 f"standalone {case_name} trace hash mismatch")
        validate(trace, case_name)
    return strict_cycles


def import_goldens(results, destination):
    root = Path(results).resolve()
    target = Path(destination).resolve()
    if target.exists():
        raise Step8ResultError(
            f"refusing to overwrite existing golden destination {target}")
    manifest = json.loads(
        (root / "result_manifest.json").read_text(encoding="utf-8"))
    target.mkdir(parents=True)
    for profile in manifest["profiles"]:
        profile_target = target / profile["name"]
        profile_target.mkdir()
        for key, filename in (
                ("trace", "trace.csv"),
                ("output", "output.csv"),
                ("manifest", "manifest.json")):
            shutil.copy2(root / profile[key], profile_target / filename)
    shutil.copy2(root / "result_manifest.json", target / "result_manifest.json")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--gem5-results", type=Path)
    parser.add_argument("--import-dir", type=Path)
    args = parser.parse_args()
    try:
        strict_cycles = validate_results(args.results, args.gem5_results)
        if args.import_dir is not None:
            import_goldens(args.results, args.import_dir)
    except (OSError, RuntimeError, ValueError, KeyError) as error:
        print(f"Step 8 result validation failed: {error}", file=sys.stderr)
        return 1
    if args.gem5_results is None:
        print("PASS Step 8 RTL artifacts; gem5 strict comparison pending")
    else:
        print(
            "PASS Step 8 gem5/RTL strict comparison "
            f"profiles={len(strict_cycles)} "
            f"cycles={sum(strict_cycles.values())}"
        )
    if args.import_dir is not None:
        print(f"validated goldens imported to {args.import_dir.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
