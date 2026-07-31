#!/usr/bin/env python3
"""Strict fixture loader for the compaction-aware streaming pipeline."""

import json
from dataclasses import replace
from pathlib import Path

from util.conv_pipeline.pipeline_contract import PipelineConfigError
from util.conv_pipeline.pipeline_fixture import (
    FixtureError,
    reject_duplicate_keys,
    resolve_fixture_fields,
)
from util.conv_pipeline.streaming_contract import validate_streaming_config
from util.conv_pipeline.streaming_contract import (
    ResolvedStreamingConfig,
    resolve_shared_spad,
    streaming_resolved_config_sha256,
)


class StreamingFixtureError(FixtureError):
    """Raised when a fixture violates the streaming exploration contract."""


_MISSING = object()


def resolve_streaming_fixture(document):
    try:
        if type(document) is not dict:
            raise StreamingFixtureError(
                "fixture root must be a JSON object")
        common_document = dict(document)
        shared_document = common_document.pop("shared_spad", _MISSING)
        if shared_document is not _MISSING and type(shared_document) is not dict:
            raise StreamingFixtureError(
                "shared_spad must be a JSON object")
        loaded = resolve_fixture_fields(common_document)
        shared = resolve_shared_spad(
            loaded.config,
            None if shared_document is _MISSING else shared_document,
        )
        config = ResolvedStreamingConfig(
            **loaded.config.__dict__,
            shared_spad=shared,
        )
        derived = validate_streaming_config(config)
    except FixtureError as error:
        raise StreamingFixtureError(str(error)) from error
    except PipelineConfigError as error:
        raise StreamingFixtureError(str(error)) from error
    return replace(
        loaded,
        config=config,
        derived=derived,
        resolved_config_sha256=streaming_resolved_config_sha256(config),
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
