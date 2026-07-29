"""Run the packaged legal-but-unintended ABTD functional fixture."""

import os
import runpy
import sys

import m5


gem5_root = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "..", "..")
)
profile = os.path.join(
    gem5_root,
    "tests",
    "gem5",
    "sau",
    "functional_ref",
    "int8_gemm_32x32x32_abtd_boundary",
)
sau_config = os.path.join(gem5_root, "configs", "example", "sau_timing.py")

sys.argv = [
    sau_config,
    f"--rtl-profile={profile}",
    f"--memory-image={os.path.join(profile, 'initial_memory.hex')}",
    "--memory-image-base=0x29120000",
    "--functional-memory-base=0x20000000",
    "--functional-memory-size=0x20000000",
    "--final-memory-dump="
    + os.path.join(m5.options.outdir, "final_output_memory.hex"),
    "--final-memory-dump-base=0x29120c00",
    "--final-memory-dump-size=1024",
]
runpy.run_path(sau_config)
