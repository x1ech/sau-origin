import filecmp
import re
import subprocess
import sys

from testlib import (
    config,
    constants,
    gem5_verify_config,
    joinpath,
    verifier,
)


class VerifyFusedRtlResult(verifier.Verifier):
    """Compare gem5 cycle and NCHW outputs against fused RTL goldens."""

    def __init__(self, profile):
        super().__init__()
        self.reference = joinpath(
            config.base_dir,
            "tests",
            "gem5",
            "conv_pipeline",
            "ref",
            profile,
        )

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        generated = joinpath(tempdir, "conv_pipeline")
        comparator = joinpath(
            config.base_dir,
            "util",
            "conv_pipeline",
            "compare_pipeline_traces.py",
        )
        result = subprocess.run(
            (
                sys.executable,
                comparator,
                joinpath(self.reference, "trace.csv"),
                joinpath(generated, "trace.csv"),
            ),
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
            raise AssertionError(
                f"Fused RTL trace comparison failed:\n{output}\n"
                f"See {tempdir} for full results"
            )

        expected_output = joinpath(self.reference, "output.csv")
        generated_output = joinpath(generated, "output.csv")
        if not filecmp.cmp(expected_output, generated_output, shallow=False):
            raise AssertionError(
                "Fused RTL NCHW output comparison failed\n"
                f"See {tempdir} for full results"
            )


config_script = joinpath(
    config.base_dir, "configs", "example", "conv_pipeline_timing.py"
)
exit_regex = re.compile(
    r"Conv pipeline timing simulation exited: conv pipeline drained"
)


def verify_fused_pipeline(name, fixture_name, ready_period=1, ready_high=1):
    fixture = joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "conv_pipeline",
        "fixtures",
        f"{fixture_name}.json",
    )
    arguments = [f"--fixture={fixture}"]
    if (ready_period, ready_high) != (1, 1):
        arguments.extend((
            f"--output-ready-period={ready_period}",
            f"--output-ready-high-cycles={ready_high}",
        ))
    gem5_verify_config(
        name=f"conv-pipeline-fused-rtl-{name}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(exit_regex),
            VerifyFusedRtlResult(fixture_name),
        ),
        config=config_script,
        config_args=arguments,
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


verify_fused_pipeline("c1-w1-oc1-ones", "01_c1_w1_oc1_ones")
verify_fused_pipeline("c2-w5-oc3-pack", "02_c2_w5_oc3_pack")
verify_fused_pipeline("c3-w16-oc16-full", "03_c3_w16_oc16_full")
verify_fused_pipeline("c3-w17-oc7-dil2", "04_c3_w17_oc7_dil2")
verify_fused_pipeline("n2-c4-w20-oc15-s2", "05_n2_c4_w20_oc15_s2")
verify_fused_pipeline("c63-w1-oc16-maxk", "06_c63_w1_oc16_maxk")
verify_fused_pipeline(
    "c2-w5-oc3-output-backpressure",
    "07_c2_w5_oc3_outbp",
    ready_period=11,
    ready_high=1,
)
