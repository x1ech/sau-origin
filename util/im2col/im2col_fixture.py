#!/usr/bin/env python3
"""Load and resolve Im2Col workload fixtures into the frozen config type."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.im2col.im2col_contract import (
    ConfigError,
    DerivedConfig,
    KERNEL_PATTERN_ALL,
    ResolvedConfig,
    canonical_config_bytes,
    checked_add,
    checked_multiply,
    resolved_config_sha256,
    validate_and_derive,
)


REQUIRED_FIELDS = {
    "schema_version",
    "name",
    "n",
    "c",
    "h",
    "w",
    "kernel_h",
    "kernel_w",
    "stride_h",
    "stride_w",
    "dilation_h",
    "dilation_w",
    "pad_top",
    "pad_left",
    "spad_base",
    "input_generator",
}
OPTIONAL_FIELDS = {
    "out_h",
    "out_w",
    "cfg_dw_mode",
    "cfg_kernel_pattern",
}
ALLOWED_FIELDS = REQUIRED_FIELDS | OPTIONAL_FIELDS


class FixtureError(ConfigError):
    """Raised when a logical fixture violates its JSON or output contract."""


@dataclass(frozen=True)
class LoadedFixture:
    config: ResolvedConfig
    derived: DerivedConfig
    warnings: tuple
    resolved_config_sha256: str


def _reject_duplicate_keys(pairs):
    document = {}
    for key, value in pairs:
        if key in document:
            raise FixtureError(f"duplicate JSON field: {key}")
        document[key] = value
    return document


def _output_extent(size, padding, kernel, stride, dilation, axis):
    effective_kernel = checked_add(
        checked_multiply(dilation, kernel - 1,
                         f"effective kernel {axis}"),
        1,
        f"effective kernel {axis}",
    )
    padded_size = checked_add(
        size,
        checked_multiply(2, padding, f"padded input {axis}"),
        f"padded input {axis}",
    )
    if padded_size < effective_kernel:
        raise FixtureError(
            f"automatic out_{axis} formula has a negative numerator")
    return (padded_size - effective_kernel) // stride + 1


def _automatic_output(config):
    return (
        _output_extent(
            config.h, config.pad_top, config.kernel_h,
            config.stride_h, config.dilation_h, "h"),
        _output_extent(
            config.w, config.pad_left, config.kernel_w,
            config.stride_w, config.dilation_w, "w"),
    )


def _config_from_document(document, out_h, out_w):
    return ResolvedConfig(
        schema_version=document["schema_version"],
        name=document["name"],
        n=document["n"],
        c=document["c"],
        h=document["h"],
        w=document["w"],
        out_h=out_h,
        out_w=out_w,
        kernel_h=document["kernel_h"],
        kernel_w=document["kernel_w"],
        stride_h=document["stride_h"],
        stride_w=document["stride_w"],
        dilation_h=document["dilation_h"],
        dilation_w=document["dilation_w"],
        pad_top=document["pad_top"],
        pad_left=document["pad_left"],
        spad_base=document["spad_base"],
        cfg_dw_mode=document.get("cfg_dw_mode", 0),
        cfg_kernel_pattern=document.get(
            "cfg_kernel_pattern", KERNEL_PATTERN_ALL),
        input_generator=document["input_generator"],
    )


def _resolve_fixture(document):
    if type(document) is not dict:
        raise FixtureError("fixture root must be a JSON object")
    if any(type(key) is not str for key in document):
        raise FixtureError("fixture field names must be strings")

    missing = sorted(REQUIRED_FIELDS - document.keys())
    if missing:
        raise FixtureError(
            "fixture is missing required fields: " + ", ".join(missing))
    unknown = sorted(document.keys() - ALLOWED_FIELDS)
    if unknown:
        raise FixtureError(
            "fixture has unknown fields: " + ", ".join(unknown))
    if type(document["name"]) is not str or not document["name"]:
        raise FixtureError("name must be a non-empty string")

    has_out_h = "out_h" in document
    has_out_w = "out_w" in document
    if has_out_h != has_out_w:
        raise FixtureError("out_h and out_w must be provided together")

    provisional = _config_from_document(
        document,
        document["out_h"] if has_out_h else 1,
        document["out_w"] if has_out_w else 1,
    )
    validate_and_derive(provisional)

    warnings = []
    if has_out_h:
        resolved = provisional
        try:
            automatic = _automatic_output(provisional)
        except FixtureError:
            automatic = None
        explicit = (resolved.out_h, resolved.out_w)
        if automatic is not None and explicit != automatic:
            warnings.append(
                "explicit output " + f"{explicit[0]}x{explicit[1]} " +
                "differs from automatic output " +
                f"{automatic[0]}x{automatic[1]}; explicit values retained")
    else:
        automatic = _automatic_output(provisional)
        resolved = _config_from_document(document, *automatic)

    derived = validate_and_derive(resolved)
    return LoadedFixture(
        config=resolved,
        derived=derived,
        warnings=tuple(warnings),
        resolved_config_sha256=resolved_config_sha256(resolved),
    )


def resolve_fixture(document):
    """Validate a decoded JSON object and produce one resolved config."""
    try:
        return _resolve_fixture(document)
    except FixtureError:
        raise
    except ConfigError as error:
        raise FixtureError(str(error)) from error


def load_fixture(path):
    """Read a fixture path and return its resolved representation."""
    fixture_path = Path(path)
    try:
        text = fixture_path.read_text(encoding="utf-8")
    except OSError as error:
        raise FixtureError(f"cannot read fixture {fixture_path}: {error}") \
            from error
    try:
        document = json.loads(text, object_pairs_hook=_reject_duplicate_keys)
    except FixtureError:
        raise
    except json.JSONDecodeError as error:
        raise FixtureError(f"invalid JSON in {fixture_path}: {error}") from error
    return resolve_fixture(document)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fixture", type=Path)
    args = parser.parse_args(argv)
    try:
        loaded = load_fixture(args.fixture)
    except FixtureError as error:
        print(error, file=sys.stderr)
        return 1

    for warning in loaded.warnings:
        print(f"warning: {warning}", file=sys.stderr)
    print(f"expected_vectors={loaded.derived.expected_vectors}",
          file=sys.stderr)
    print(canonical_config_bytes(loaded.config).decode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
