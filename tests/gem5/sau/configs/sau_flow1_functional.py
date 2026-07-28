"""Run the packaged PLAN3 Flow1 functional fixture in the test outdir."""

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
    "int8_gemm_64x160x64_atbd_flow1_cutbit8",
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
    "--final-memory-dump-base=0x29126000",
    "--final-memory-dump-size=4096",
]
runpy.run_path(sau_config)
