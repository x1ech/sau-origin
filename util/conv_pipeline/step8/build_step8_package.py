#!/usr/bin/env python3
"""Build a deterministic self-contained Step 8 workstation archive."""

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

from util.conv_pipeline.step8.verify_step8_sources import verify


PACKAGE_ROOT = "sau_n_step8_fused_workstation_20260720"


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def selected_files(root):
    files = [
        root / "src/sau_n/SAU_PLAN.md",
        root / "src/sau_n/SAU_FREEZE_SCOPE.md",
        root / "src/sau_n/FUSED_RTL_VALIDATION_20260720.json",
    ]
    files.extend(
        path for path in (root / "src/sau_n/rtl").rglob("*")
        if path.is_file() and path.suffix.lower() != ".pdf"
    )
    files.extend(
        path for path in (root / "util/conv_pipeline").rglob("*")
        if path.is_file() and "__pycache__" not in path.parts and
        path.suffix != ".pyc"
    )
    files.extend(
        path for path in (root / "util/im2col").glob("*.py")
        if path.is_file()
    )
    files.extend(
        path for path in (root / "tests/gem5/conv_pipeline").rglob("*")
        if path.is_file()
    )
    result = sorted(
        set(files), key=lambda path: path.relative_to(root).as_posix())
    for path in result:
        if not path.is_file():
            raise FileNotFoundError(path)
    return result


def add_bytes(archive, arcname, data, mode):
    info = tarfile.TarInfo(arcname)
    info.size = len(data)
    info.mode = mode
    info.mtime = 0
    info.uid = 0
    info.gid = 0
    info.uname = "root"
    info.gname = "root"
    archive.addfile(info, io.BytesIO(data))


def build(root, output):
    verify(root)
    if output.exists():
        raise FileExistsError(f"refusing to overwrite {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    paths = selected_files(root)
    payloads = {
        path.relative_to(root).as_posix(): path.read_bytes()
        for path in paths
    }
    checksums = "".join(
        f"{sha256_bytes(data)}  {relative}\n"
        for relative, data in sorted(payloads.items())
    ).encode("utf-8")
    metadata = (
        json.dumps({
            "schema_version": 1,
            "package_root": PACKAGE_ROOT,
            "golden_object": (
                "validated Im2Col plus project-owned sau_array_16x16 fusion"
            ),
            "pipeline_profiles": 7,
            "standalone_profiles": [
                "tail_r1_c1_k9", "tail_r15_c15_k9",
                "full_r16_c16_k9", "backpressure_r3_c3_k9",
                "sat_pos_r1_c1_k567", "sat_neg_r1_c1_k567"],
            "includes_legacy_im2col_regression": True,
            "rtl_functional_validation": "passed",
            "final_gem5_strict_comparison": (
                "pending in complete local gem5 source tree"
            ),
        }, indent=2, sort_keys=True) + "\n"
    ).encode("utf-8")

    with output.open("xb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) \
                as zipped:
            with tarfile.open(fileobj=zipped, mode="w") as archive:
                for relative, data in sorted(payloads.items()):
                    path = root / relative
                    add_bytes(
                        archive, f"{PACKAGE_ROOT}/{relative}", data,
                        path.stat().st_mode & 0o777)
                add_bytes(
                    archive, f"{PACKAGE_ROOT}/SHA256SUMS", checksums, 0o644)
                add_bytes(
                    archive, f"{PACKAGE_ROOT}/PACKAGE_METADATA.json",
                    metadata, 0o644)

    with tarfile.open(output, "r:gz") as archive:
        for relative, expected in payloads.items():
            member = archive.extractfile(f"{PACKAGE_ROOT}/{relative}")
            if member is None or member.read() != expected:
                raise RuntimeError(f"archive self-check failed: {relative}")
        member = archive.extractfile(f"{PACKAGE_ROOT}/SHA256SUMS")
        if member is None or member.read() != checksums:
            raise RuntimeError("archive SHA256SUMS self-check failed")
    return sha256_file(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=SCRIPT_ROOT)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        digest = build(args.root.resolve(), args.output.resolve())
    except (OSError, RuntimeError) as error:
        print(f"Step 8 package build failed: {error}", file=sys.stderr)
        return 1
    print(f"PASS Step 8 package {args.output.resolve()} sha256={digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
