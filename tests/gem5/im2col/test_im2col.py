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


class VerifyIm2ColRtlTrace(verifier.Verifier):
    """Compare the generated gem5 trace against one validated RTL golden."""

    def __init__(
            self, rtl_profile, done_cycle, drained_cycle, post_done_cycles):
        super().__init__()
        self.rtl_trace = joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "im2col",
            "ref",
            rtl_profile,
            "trace.csv",
        )
        self.expected_stats = {
            "rtlDoneCycle": done_cycle,
            "drainedCycle": drained_cycle,
            "postDoneDrainCycles": post_done_cycles,
        }

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        trace = joinpath(tempdir, "im2col", "trace.csv")
        comparator = joinpath(
            config.base_dir, "util", "im2col", "compare_traces.py"
        )
        result = subprocess.run(
            (sys.executable, comparator, self.rtl_trace, trace),
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
                f"Im2Col RTL trace comparison failed:\n{output}\n"
                f"See {tempdir} for full results"
            )
        stats_path = joinpath(tempdir, "stats.txt")
        with open(stats_path, encoding="utf-8") as stats_file:
            stats = stats_file.read()
        for field, expected in self.expected_stats.items():
            pattern = re.compile(
                rf"(?m)^im2col\.{field}\s+{expected}\s+"
            )
            if not pattern.search(stats):
                test_util.fail(
                    f"Im2Col stat {field} did not equal {expected}\n"
                    f"See {stats_path} for full results"
                )


config_script = joinpath(
    config.base_dir, "configs", "example", "im2col_timing.py"
)
exit_regex = re.compile(
    r"Im2Col timing simulation exited: im2col model drained"
)


def verify_im2col(
        name, fixture_name, rtl_profile, config_args,
        done_cycle, drained_cycle, post_done_cycles):
    fixture = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "im2col",
        "fixtures",
        fixture_name,
    )
    gem5_verify_config(
        name=name,
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(exit_regex),
            VerifyIm2ColRtlTrace(
                rtl_profile, done_cycle, drained_cycle, post_done_cycles
            ),
        ),
        config=config_script,
        config_args=[f"--fixture={fixture}", *config_args],
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


verify_im2col(
    "im2col-rtl-w1-n2-c2-base7",
    "w1_n2_c2_base7.json",
    "00_w1_n2_c2_base7_p1_h1",
    [],
    21,
    21,
    0,
)
verify_im2col(
    "im2col-rtl-w5-default-ready",
    "w5_pack3_pad1.json",
    "01_w5_pack3_pad1_p1_h1",
    [],
    175,
    175,
    0,
)
verify_im2col(
    "im2col-rtl-w16-explicit-tail",
    "w16_explicit_tail.json",
    "02_w16_explicit_tail_p1_h1",
    [],
    11,
    11,
    0,
)
verify_im2col(
    "im2col-rtl-w17-dilation-tail",
    "w17_dilation_tail.json",
    "03_w17_dilation_tail_p1_h1",
    [],
    109,
    109,
    0,
)
verify_im2col(
    "im2col-rtl-w20-bank-conflict",
    "w20_stride2_conflict.json",
    "04_w20_stride2_conflict_p1_h1",
    [],
    301,
    301,
    0,
)
verify_im2col(
    "im2col-rtl-w5-all-padding",
    "w5_all_padding.json",
    "05_w5_all_padding_p1_h1",
    [],
    5,
    5,
    0,
)
verify_im2col(
    "im2col-rtl-w5-periodic-ready",
    "w5_pack3_pad1.json",
    "06_w5_pack3_pad1_p11_h1",
    ["--ready-period=11", "--ready-high-cycles=1"],
    356,
    397,
    41,
)
