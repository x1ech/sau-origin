#!/usr/bin/env python3
"""Verify Step 8 RTL integration sources and frozen matrix wiring."""

import argparse
import json
from pathlib import Path
import re

if __package__ in (None, ""):
    import sys
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from util.conv_pipeline.compare_pipeline_traces import TRACE_FIELDS
from util.conv_pipeline.pipeline_fixture import load_fixture
from util.conv_pipeline.rtl_pipeline_runner import RTL_SOURCES
from util.conv_pipeline.step0.verify_step0_sources import verify as verify_step0


EXPECTED_PIPELINE_FILELIST = (
    "+incdir+src/sau_n/rtl/mikui/original",
    "src/sau_n/rtl/mikui/original/SA_pkg.sv",
    "src/sau_n/rtl/mikui/original/DW02_mult_2_stage.v",
    "src/sau_n/rtl/mikui/original/active_delay.v",
    "src/sau_n/rtl/mikui/original/weight_delay.v",
    "src/sau_n/rtl/mikui/original/SA_PE.sv",
    "src/sau_n/rtl/mikui/original/SA_ROW.sv",
    "src/sau_n/rtl/mikui/integration/SA_ENGINE.sv",
    "src/sau_n/rtl/gemmini_im2col_chw_gather_readable.sv",
    "src/sau_n/rtl/im2col_mikui_sau_pipeline.sv",
    "src/sau_n/rtl/tb_im2col_mikui_sau_pipeline.sv",
)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def _trace_header(testbench):
    match = re.search(
        r"task automatic write_trace_header;(.*?)endtask",
        testbench,
        flags=re.DOTALL,
    )
    require(match is not None, "pipeline testbench has no trace-header task")
    literals = re.findall(r'"([^"\\]*(?:\\.[^"\\]*)*)"', match.group(1))
    require(literals, "pipeline trace-header task contains no string literals")
    return bytes("".join(literals), "utf-8").decode("unicode_escape")


def verify(root):
    root = Path(root).resolve()
    verify_step0(root)
    for relative in RTL_SOURCES:
        require((root / relative).is_file(), f"missing RTL source: {relative}")

    filelist_path = root / "src/sau_n/rtl/mikui/filelists/pipeline.f"
    filelist = tuple(filelist_path.read_text(encoding="utf-8").splitlines())
    require(
        filelist == EXPECTED_PIPELINE_FILELIST,
        "pipeline filelist differs from the frozen ordered source list",
    )
    for entry in filelist:
        if not entry.startswith("+"):
            require(
                (root / entry).is_file(),
                f"missing filelist entry: {entry}",
            )

    testbench = (
        root / "src/sau_n/rtl/tb_im2col_mikui_sau_pipeline.sv"
    ).read_text(encoding="utf-8")
    require(
        _trace_header(testbench) == ",".join(TRACE_FIELDS) + "\n",
        "pipeline RTL trace header differs from the canonical 53-field schema",
    )

    matrix_path = root / "tests/gem5/conv_pipeline/golden_matrix.json"
    matrix = json.loads(matrix_path.read_text(encoding="utf-8"))
    require(matrix.get("schema_version") == 1, "matrix schema must be 1")
    profiles = matrix.get("profiles")
    require(
        isinstance(profiles, list) and len(profiles) == 7,
        "Step 8 matrix must contain exactly seven profiles",
    )
    names = []
    for profile in profiles:
        require(
            set(profile) == {
                "name", "fixture", "output_ready_period",
                "output_ready_high_cycles",
            },
            "matrix profile fields differ from the frozen schema",
        )
        names.append(profile["name"])
        load_fixture(root / profile["fixture"])
        period = profile["output_ready_period"]
        high = profile["output_ready_high_cycles"]
        require(
            type(period) is int and type(high) is int and
            period >= 1 and 1 <= high <= period,
            f"invalid output-ready pattern for {profile['name']}",
        )
    require(len(set(names)) == 7, "matrix profile names must be unique")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", type=Path,
        default=Path(__file__).resolve().parents[3],
    )
    args = parser.parse_args()
    verify(args.root)
    print(
        f"PASS Step 8 sources root={args.root.resolve()} "
        "profiles=7 fields=53"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
