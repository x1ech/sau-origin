#!/usr/bin/env python3
"""Strict nested JSON fixture loader for the convolution pipeline."""

import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import sys

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from util.conv_pipeline.pipeline_contract import (
    DerivedPipelineConfig,
    PipelineConfigError,
    ResolvedPipelineConfig,
    canonical_config_bytes,
    resolved_config_sha256,
    validate_and_derive,
)
from util.im2col.im2col_fixture import (
    FixtureError as Im2ColFixtureError,
    resolve_fixture as resolve_im2col_fixture,
)
from util.conv_pipeline.tile_mapping import (
    TileMappingError,
    spatial_tiles,
)


REQUIRED_FIELDS = {
    "schema_version",
    "name",
    "im2col",
    "out_channels",
    "cutbit",
    "weight_generator",
    "bias_generator",
}


class FixtureError(PipelineConfigError):
    """Raised when a pipeline fixture violates its strict schema."""


@dataclass(frozen=True)
class LoadedFixture:
    config: ResolvedPipelineConfig
    derived: DerivedPipelineConfig
    warnings: tuple
    resolved_config_sha256: str


def _reject_duplicate_keys(pairs):
    document = {}
    for key, value in pairs:
        if key in document:
            raise FixtureError(f"duplicate JSON field: {key}")
        document[key] = value
    return document


def _resolve_fixture(document):
    if type(document) is not dict:
        raise FixtureError("fixture root must be a JSON object")
    if any(type(key) is not str for key in document):
        raise FixtureError("fixture field names must be strings")
    missing = sorted(REQUIRED_FIELDS - document.keys())
    if missing:
        raise FixtureError(
            "fixture is missing required fields: " + ", ".join(missing))
    unknown = sorted(document.keys() - REQUIRED_FIELDS)
    if unknown:
        raise FixtureError(
            "fixture has unknown fields: " + ", ".join(unknown))
    if type(document["name"]) is not str or not document["name"]:
        raise FixtureError("name must be a non-empty string")

    try:
        im2col = resolve_im2col_fixture(document["im2col"])
    except Im2ColFixtureError as error:
        raise FixtureError(f"im2col.{error}") from error
    resolved = ResolvedPipelineConfig(
        schema_version=document["schema_version"],
        name=document["name"],
        im2col=im2col.config,
        out_channels=document["out_channels"],
        cutbit=document["cutbit"],
        weight_generator=document["weight_generator"],
        bias_generator=document["bias_generator"],
    )
    derived = validate_and_derive(resolved)
    try:
        spatial_tiles(resolved)
    except TileMappingError as error:
        raise FixtureError(str(error)) from error
    return LoadedFixture(
        config=resolved,
        derived=derived,
        warnings=tuple(f"im2col: {warning}" for warning in im2col.warnings),
        resolved_config_sha256=resolved_config_sha256(resolved),
    )


def resolve_fixture(document):
    try:
        return _resolve_fixture(document)
    except FixtureError:
        raise
    except PipelineConfigError as error:
        raise FixtureError(str(error)) from error


def load_fixture(path):
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
        raise FixtureError(f"invalid JSON in {fixture_path}: {error}") \
            from error
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
    print(f"expected_tiles={loaded.derived.expected_tiles}", file=sys.stderr)
    print(f"expected_outputs={loaded.derived.expected_outputs}", file=sys.stderr)
    print(f"expected_macs={loaded.derived.expected_macs}", file=sys.stderr)
    print(f"resolved_config_sha256={loaded.resolved_config_sha256}",
          file=sys.stderr)
    print(canonical_config_bytes(loaded.config).decode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
