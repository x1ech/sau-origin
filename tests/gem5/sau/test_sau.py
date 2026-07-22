import os
import re
import subprocess
import sys

from testlib import (
    config,
    constants,
    gem5_verify_config,
    joinpath,
    test_util,
    verifier,
)


exit_regex = re.compile(r"SAU timing simulation exited: SAU command complete")


class VerifySauStrictTraces(verifier.Verifier):
    """Compare SAU architecture and semantic-state traces against RTL."""

    def __init__(self, rtl_profile):
        super().__init__()
        self.rtl_profile = rtl_profile

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        comparisons = (
            (
                "architecture",
                joinpath(config.base_dir, "util", "sau", "compare_trace.py"),
                (
                    "--mode",
                    "strict",
                    joinpath(self.rtl_profile, "architecture.csv"),
                    joinpath(tempdir, "sau.csv"),
                ),
            ),
            (
                "semantic state",
                joinpath(
                    config.base_dir,
                    "util",
                    "sau",
                    "compare_state_trace.py",
                ),
                (
                    "--rtl-diagnostic",
                    joinpath(self.rtl_profile, "diagnostic.csv"),
                    joinpath(tempdir, "sau_state.csv"),
                    "--max-errors",
                    "20",
                ),
            ),
        )

        for label, comparator, arguments in comparisons:
            result = subprocess.run(
                (sys.executable, comparator, *arguments),
                cwd=config.base_dir,
                check=False,
                capture_output=True,
                text=True,
            )
            if result.returncode:
                output = "\n".join(
                    part.rstrip()
                    for part in (result.stdout, result.stderr)
                    if part
                )
                test_util.fail(
                    f"SAU {label} strict comparison failed for "
                    f"{os.path.basename(self.rtl_profile)}:\n{output}\n"
                    f"See {tempdir} for full results"
                )


def verify_sau_config(name, config_args):
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(verifier.MatchRegex(exit_regex),),
        config=joinpath(
            config.base_dir, "configs", "example", "sau_timing.py"
        ),
        config_args=config_args,
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


def verify_sau_rtl_profile(name):
    rtl_profile = joinpath(
        config.base_dir, "tests", "gem5", "sau", "ref", name
    )
    gem5_verify_config(
        name=f"sau-strict-{name}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(exit_regex),
            VerifySauStrictTraces(rtl_profile),
        ),
        config=joinpath(
            config.base_dir, "configs", "example", "sau_timing.py"
        ),
        config_args=[f"--rtl-profile={rtl_profile}"],
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


# The eight profiles previously registered here were captured from a retired
# scheduler/result RTL baseline. Keep the files as historical diagnostics, but
# do not use them as strict acceptance oracles. New profiles may be registered
# only after their source hashes and simulator build match
# src/sau/RTL_TIMING_PROVENANCE.md.


verify_sau_config(
    "sau-timing-fixed-memory",
    [
        "--memory-latency=3ns",
        "--memory-latency-var=0ns",
        "--memory-bandwidth=256GiB/s",
    ],
)

verify_sau_config(
    "sau-timing-constrained-memory",
    [
        "--memory-latency=20ns",
        "--memory-latency-var=5ns",
        "--memory-bandwidth=1GiB/s",
        "--max-outstanding-reads=2",
        "--max-outstanding-writes=2",
    ],
)


def dse_args(*resource_args):
    return [
        "--a-beats=1",
        "--b-beats=1",
        "--output-beats=16",
        "--flow-loops=16",
        "--array-fill-cycles=2",
        "--array-input-start-delay-cycles=0",
        "--array-input-burst-beats=1",
        "--array-input-burst-gap-cycles=0",
        "--array-input-flow-gap-cycles=0",
        "--result-flow-gap-cycles=0",
        "--writeback-start-delay-cycles=0",
    ] + list(resource_args)


verify_sau_config(
    "sau-dse-array-capacity-1",
    dse_args("--array-capacity=1", "--output-buffer-entries=16"),
)

verify_sau_config(
    "sau-dse-array-capacity-16",
    dse_args("--array-capacity=16", "--output-buffer-entries=16"),
)

verify_sau_config(
    "sau-dse-output-buffer-1",
    dse_args(
        "--array-capacity=16",
        "--output-buffer-entries=1",
        "--memory-latency=20ns",
        "--max-outstanding-writes=1",
    ),
)

verify_sau_config(
    "sau-dse-output-buffer-8",
    dse_args(
        "--array-capacity=16",
        "--output-buffer-entries=8",
        "--memory-latency=20ns",
        "--max-outstanding-writes=1",
    ),
)
