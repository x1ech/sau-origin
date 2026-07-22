#!/usr/bin/env python3
"""Strict fixture loader for the compaction-aware streaming pipeline."""

import json
from pathlib import Path

from util.conv_pipeline.pipeline_contract import PipelineConfigError
from util.conv_pipeline.pipeline_fixture import (
    FixtureError,
    reject_duplicate_keys,
    resolve_fixture_fields,
)
from util.conv_pipeline.streaming_contract import validate_streaming_config


class StreamingFixtureError(FixtureError):
    """Raised when a fixture violates the streaming exploration contract."""


def resolve_streaming_fixture(document):
    try:
        loaded = resolve_fixture_fields(document)
        derived = validate_streaming_config(loaded.config)
    except FixtureError as error:
        raise StreamingFixtureError(str(error)) from error
    except PipelineConfigError as error:
        raise StreamingFixtureError(str(error)) from error
    return type(loaded)(
        config=loaded.config,
        derived=derived,
        warnings=loaded.warnings,
        resolved_config_sha256=loaded.resolved_config_sha256,
    )


def load_streaming_fixture(path):
    fixture_path = Path(path)
    try:
        text = fixture_path.read_text(encoding="utf-8")
    except OSError as error:
        raise StreamingFixtureError(
            f"cannot read fixture {fixture_path}: {error}") from error
    try:
        document = json.loads(text, object_pairs_hook=reject_duplicate_keys)
    except FixtureError as error:
        raise StreamingFixtureError(str(error)) from error
    except json.JSONDecodeError as error:
        raise StreamingFixtureError(
            f"invalid JSON in {fixture_path}: {error}") from error
    return resolve_streaming_fixture(document)
