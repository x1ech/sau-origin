#!/usr/bin/env python3
"""Build a deterministic, self-contained Step 0 workstation archive."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import json
from pathlib import Path
import sys
import tarfile

SCRIPT_ROOT = Path(__file__).resolve().parents[3]
if str(SCRIPT_ROOT) not in sys.path:
    sys.path.insert(0, str(SCRIPT_ROOT))
from util.conv_pipeline.step0.verify_step0_sources import FULL_COMMIT, verify


PACKAGE_ROOT = "sau_n_step0_workstation_20260718"


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_files(root: Path) -> list[Path]:
    paths = [
        root / "src/sau_n/SAU_PLAN.md",
        root / "src/sau_n/rtl/tb_mikui_sau_engine_step0.sv",
    ]
    paths.extend(
        path
        for path in (root / "src/sau_n/rtl/mikui").rglob("*")
        if path.is_file()
    )
    paths.extend(
        path
        for path in (root / "util/conv_pipeline/step0").glob("*")
        if path.is_file() and path.suffix != ".pyc"
    )
    unique = sorted(set(paths), key=lambda path: path.relative_to(root).as_posix())
    for path in unique:
        if not path.is_file():
            raise FileNotFoundError(path)
    return unique


def add_bytes(
    archive: tarfile.TarFile, arcname: str, data: bytes, mode: int
) -> None:
    info = tarfile.TarInfo(arcname)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    archive.addfile(info, io.BytesIO(data))


def build(root: Path, output: Path) -> None:
    verify(root)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite {output}")
    output.parent.mkdir(parents=True, exist_ok=True)

    files = selected_files(root)
    payloads = {
        path.relative_to(root).as_posix(): path.read_bytes() for path in files
    }
    checksums = "".join(
        f"{sha256_bytes(data)}  {relative}\n"
        for relative, data in sorted(payloads.items())
    ).encode("utf-8")
    metadata = (
        json.dumps(
            {
                "schema_version": 1,
                "package_root": PACKAGE_ROOT,
                "upstream_commit": FULL_COMMIT,
                "golden_object": (
                    "2ca8252 with documented FINISH_ROW/FINISH_COL width patch"
                ),
                "matrix_variants": ["original", "integration"],
                "matrix_cases": [
                    "tail_r1_c1",
                    "tail_r15_c15",
                    "tail_r16_c16",
                    "mapping_k9",
                    "control_k9_bp",
                    "sat_pos_k567",
                    "sat_neg_k567",
                ],
            },
            indent=2,
            sort_keys=True,
        )
        + "\n"
    ).encode("utf-8")

    with output.open("xb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as archive:
                for relative, data in sorted(payloads.items()):
                    path = root / relative
                    add_bytes(
                        archive,
                        f"{PACKAGE_ROOT}/{relative}",
                        data,
                        path.stat().st_mode & 0o777,
                    )
                add_bytes(
                    archive,
                    f"{PACKAGE_ROOT}/SHA256SUMS",
                    checksums,
                    0o644,
                )
                add_bytes(
                    archive,
                    f"{PACKAGE_ROOT}/PACKAGE_METADATA.json",
                    metadata,
                    0o644,
                )

    with tarfile.open(output, mode="r:gz") as archive:
        for relative, expected in payloads.items():
            member = archive.extractfile(f"{PACKAGE_ROOT}/{relative}")
            if member is None or member.read() != expected:
                raise RuntimeError(f"archive self-check failed: {relative}")
        checksum_member = archive.extractfile(f"{PACKAGE_ROOT}/SHA256SUMS")
        if checksum_member is None or checksum_member.read() != checksums:
            raise RuntimeError("archive SHA256SUMS self-check failed")

    print(f"PASS Step 0 package {output} sha256={sha256_file(output)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=SCRIPT_ROOT)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    build(args.root.resolve(), args.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
