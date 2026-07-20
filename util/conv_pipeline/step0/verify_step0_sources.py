#!/usr/bin/env python3
"""Verify frozen Mikui sources and the single approved integration patch."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


FULL_COMMIT = "2ca8252ef1cac43ef843998e9e08023259ac17ee"
SOURCE_DIR = "hardware/src/sa_execute"
EXPECTED_FILES = {
    "SA_pkg.sv",
    "SA_ENGINE.sv",
    "SA_ROW.sv",
    "SA_PE.sv",
    "active_delay.v",
    "weight_delay.v",
    "DW02_mult_2_stage.v",
    "registers.svh",
}
OLD_FINISH = (
    "    assign      FINISH_ROW = "
    "(($clog2(COL_NUM))'(ROW_NUM)>=row_num_i)?row_num_i:ROW_NUM;\n"
    "    assign      FINISH_COL = "
    "(($clog2(COL_NUM))'(COL_NUM)>=col_num_i)?col_num_i:COL_NUM;"
)
NEW_FINISH = (
    "    localparam logic [$clog2(ROW_NUM):0] ROW_NUM_VALUE = ROW_NUM;\n"
    "    localparam logic [$clog2(COL_NUM):0] COL_NUM_VALUE = COL_NUM;\n"
    "    assign      FINISH_ROW =\n"
    "        (ROW_NUM_VALUE >= row_num_i) ? row_num_i : ROW_NUM_VALUE;\n"
    "    assign      FINISH_COL =\n"
    "        (COL_NUM_VALUE >= col_num_i) ? col_num_i : COL_NUM_VALUE;"
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def verify(root: Path) -> None:
    mikui = root / "src/sau_n/rtl/mikui"
    provenance_path = mikui / "provenance.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8"))

    upstream = provenance["upstream"]
    require(upstream["commit"] == FULL_COMMIT, "upstream commit mismatch")
    require(
        upstream["source_directory"] == SOURCE_DIR,
        "upstream source directory mismatch",
    )

    entries = provenance["original_files"]
    names = {Path(entry["path"]).name for entry in entries}
    require(names == EXPECTED_FILES, f"unexpected original file set: {names}")
    require(len(entries) == len(EXPECTED_FILES), "duplicate provenance entries")
    for entry in entries:
        source_path = Path(entry["path"])
        require(
            source_path.parent.as_posix() == SOURCE_DIR,
            f"ambiguous source path: {source_path}",
        )
        local_path = mikui / "original" / source_path.name
        require(local_path.is_file(), f"missing original file: {local_path}")
        require(
            sha256(local_path) == entry["sha256"],
            f"original hash mismatch: {local_path}",
        )

    integration = provenance["integration"]
    patch_path = mikui / integration["patch"]
    patched_path = mikui / integration["patched_file"]
    require(sha256(patch_path) == integration["patch_sha256"], "patch hash mismatch")
    require(
        sha256(patched_path) == integration["patched_sha256"],
        "patched engine hash mismatch",
    )

    original_engine = (mikui / "original/SA_ENGINE.sv").read_text(
        encoding="utf-8"
    )
    require(
        original_engine.count(OLD_FINISH) == 1,
        "original FINISH assignment block is not unique",
    )
    expected_patched = original_engine.replace(OLD_FINISH, NEW_FINISH)
    actual_patched = patched_path.read_text(encoding="utf-8")
    require(
        actual_patched == expected_patched,
        "integration engine contains changes beyond the approved FINISH fix",
    )

    combined = "\n".join(
        (mikui / "original" / name).read_text(encoding="utf-8")
        for name in sorted(EXPECTED_FILES)
    )
    for macro in ("MODULE_TEST", "FULL_PRECISION", "MAX_USE"):
        require(
            re.search(rf"^\s*`define\s+{macro}\b", combined, re.MULTILINE)
            is None,
            f"production macro unexpectedly defined: {macro}",
        )

    package = (mikui / "original/SA_pkg.sv").read_text(encoding="utf-8")
    engine = original_engine
    multiplier = (mikui / "original/DW02_mult_2_stage.v").read_text(
        encoding="utf-8"
    )
    require("OUTPUTDW = 24" in package, "SA_pkg OUTPUTDW is not 24")
    require("SA_SIZE = 16" in package, "SA_pkg SA_SIZE is not 16")
    require("CONV = 2'b01" in package, "SA_pkg CONV encoding is not 2'b01")
    require("IntFormat    = SA_pkg::INT8" in engine, "engine input is not INT8")
    require("IntFormat_q  = SA_pkg::INT16" in engine, "engine quant slot is not INT16")
    require(
        multiplier.count("PRODUCT <= pre_product;") == 1,
        "unexpected multiplier output-register implementation",
    )

    for variant in ("original", "integration"):
        filelist = mikui / "filelists" / f"{variant}.f"
        require(filelist.is_file(), f"missing filelist: {filelist}")
        for line in filelist.read_text(encoding="utf-8").splitlines():
            if not line or line.startswith("+"):
                continue
            require((root / line).is_file(), f"filelist entry missing: {line}")


def main() -> int:
    default_root = Path(__file__).resolve().parents[3]
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=default_root)
    args = parser.parse_args()
    verify(args.root.resolve())
    print(f"PASS Step 0 sources root={args.root.resolve()} commit={FULL_COMMIT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
