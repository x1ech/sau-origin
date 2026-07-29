#!/usr/bin/env python3
"""Validate a paired SAU functional fixture contract.

The pair must use the same input memory, matrix, elaboration parameters,
output range, and CSR modes except for ``trans_mode``.  Its RTL final-memory
oracles must differ, proving that the legal-but-unintended configuration is
not normalized back to the baseline mode.
"""

import argparse
import json
import sys
from pathlib import Path


class FunctionalPairError(Exception):
    """Raised when two functional fixtures do not form the frozen pair."""


MEMORY_FIELDS = {
    "external_beat_bytes",
    "output_base",
    "output_end_exclusive",
    "output_size_bytes",
}


def load_manifest(directory):
    path = directory / "manifest.json"
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise FunctionalPairError(f"{path}: {error}") from error


def selected_fields(mapping, names):
    return {name: mapping.get(name) for name in names}


def validate_pair(baseline, candidate, baseline_trans=1, candidate_trans=2):
    """Return pair-contract errors for two functional fixture directories."""
    errors = []
    baseline_manifest = load_manifest(baseline)
    candidate_manifest = load_manifest(candidate)

    baseline_modes = dict(baseline_manifest.get("csr_modes", {}))
    candidate_modes = dict(candidate_manifest.get("csr_modes", {}))
    actual_baseline_trans = baseline_modes.pop("trans_mode", None)
    actual_candidate_trans = candidate_modes.pop("trans_mode", None)
    if actual_baseline_trans != baseline_trans:
        errors.append(
            f"{baseline}: trans_mode is {actual_baseline_trans}, "
            f"expected {baseline_trans}"
        )
    if actual_candidate_trans != candidate_trans:
        errors.append(
            f"{candidate}: trans_mode is {actual_candidate_trans}, "
            f"expected {candidate_trans}"
        )
    if baseline_modes != candidate_modes:
        errors.append("CSR modes other than trans_mode differ")

    for field in ("matrix", "elaboration_params"):
        if baseline_manifest.get(field) != candidate_manifest.get(field):
            errors.append(f"{field} differs")

    baseline_memory = selected_fields(
        baseline_manifest.get("memory_contract", {}), MEMORY_FIELDS
    )
    candidate_memory = selected_fields(
        candidate_manifest.get("memory_contract", {}), MEMORY_FIELDS
    )
    if baseline_memory != candidate_memory:
        errors.append("memory output contract differs")

    try:
        baseline_input = (baseline / "initial_memory.hex").read_bytes()
        candidate_input = (candidate / "initial_memory.hex").read_bytes()
        baseline_output = (baseline / "final_output_memory.hex").read_bytes()
        candidate_output = (candidate / "final_output_memory.hex").read_bytes()
    except OSError as error:
        raise FunctionalPairError(str(error)) from error

    if baseline_input != candidate_input:
        errors.append("initial memory images differ")
    if baseline_output == candidate_output:
        errors.append("RTL final-memory oracles are identical")
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--baseline-trans", type=lambda value: int(value, 0),
                        default=1)
    parser.add_argument("--candidate-trans", type=lambda value: int(value, 0),
                        default=2)
    args = parser.parse_args(argv)

    try:
        errors = validate_pair(
            args.baseline,
            args.candidate,
            args.baseline_trans,
            args.candidate_trans,
        )
    except FunctionalPairError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    for error in errors:
        print(error)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
