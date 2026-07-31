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


class VerifyStreamingResult(verifier.Verifier):
    """Check trace, stats and output with independent Python oracles."""

    def __init__(
            self, fixture, reference_profile=None,
            output_ready_period=1, output_ready_high_cycles=1,
            expect_conflicts=False,
            expect_scattered=False, expect_full_exchange=False,
            expect_input_bubbles=False,
            expect_output_backpressure=False, expect_b_refill=False,
            expect_weight_reuse=False, expect_depth_one_baseline=False,
            expect_full_ab_conflict=False):
        super().__init__()
        self.fixture = fixture
        self.reference_profile = reference_profile
        self.output_ready_period = output_ready_period
        self.output_ready_high_cycles = output_ready_high_cycles
        self.flags = {
            "--expect-conflicts": expect_conflicts,
            "--expect-scattered": expect_scattered,
            "--expect-full-exchange": expect_full_exchange,
            "--expect-input-bubbles": expect_input_bubbles,
            "--expect-output-backpressure": expect_output_backpressure,
            "--expect-b-refill": expect_b_refill,
            "--expect-weight-reuse": expect_weight_reuse,
            "--expect-depth-one-baseline": expect_depth_one_baseline,
            "--expect-full-ab-conflict": expect_full_ab_conflict,
        }

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        generated = joinpath(tempdir, "streaming_conv_pipeline")
        command = [
            sys.executable,
            joinpath(
                config.base_dir,
                "util",
                "conv_pipeline",
                "verify_streaming_pipeline.py",
            ),
            f"--fixture={self.fixture}",
            f"--trace={joinpath(generated, 'trace.csv')}",
            f"--output={joinpath(generated, 'output.csv')}",
            f"--stats={joinpath(tempdir, 'stats.txt')}",
            f"--output-ready-period={self.output_ready_period}",
            (
                "--output-ready-high-cycles="
                f"{self.output_ready_high_cycles}"
            ),
        ]
        if self.reference_profile:
            command.append(
                "--reference-output=" + joinpath(
                    config.base_dir,
                    "tests",
                    "gem5",
                    "conv_pipeline",
                    "ref",
                    self.reference_profile,
                    "output.csv",
                )
            )
        command.extend(flag for flag, enabled in self.flags.items() if enabled)
        result = subprocess.run(
            command,
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
                f"Streaming pipeline verification failed:\n{output}\n"
                f"See {tempdir} for full results"
            )


config_script = joinpath(
    config.base_dir,
    "configs",
    "example",
    "streaming_conv_pipeline_timing.py",
)
exit_regex = re.compile(
    r"Streaming conv pipeline simulation exited: "
    r"streaming conv pipeline drained"
)


def fixture_path(group, name):
    return joinpath(
        config.base_dir,
        "tests",
        "gem5",
        group,
        "fixtures",
        f"{name}.json",
    )


def verify_streaming(
        name, group, fixture_name, reference_profile=None,
        ready_period=1, ready_high=1, **expectations):
    fixture = fixture_path(group, fixture_name)
    arguments = [f"--fixture={fixture}"]
    if (ready_period, ready_high) != (1, 1):
        arguments.extend((
            f"--output-ready-period={ready_period}",
            f"--output-ready-high-cycles={ready_high}",
        ))
    gem5_verify_config(
        name=f"streaming-conv-pipeline-{name}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(exit_regex),
            VerifyStreamingResult(
                fixture,
                reference_profile=reference_profile,
                output_ready_period=ready_period,
                output_ready_high_cycles=ready_high,
                **expectations,
            ),
        ),
        config=config_script,
        config_args=arguments,
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


verify_streaming(
    "w1-oc1", "conv_pipeline", "01_c1_w1_oc1_ones",
    reference_profile="01_c1_w1_oc1_ones",
)
verify_streaming(
    "w5-pack", "conv_pipeline", "02_c2_w5_oc3_pack",
    reference_profile="02_c2_w5_oc3_pack",
)
verify_streaming(
    "w16-oc16", "conv_pipeline", "03_c3_w16_oc16_full",
    reference_profile="03_c3_w16_oc16_full",
)
verify_streaming(
    "w17-tail-oc7", "streaming_conv_pipeline",
    "w17_stride1_tail_oc7",
)
verify_streaming(
    "w5-pad0", "streaming_conv_pipeline", "w5_stride1_pad0",
    expect_scattered=True,
)
verify_streaming(
    "n2-w20-stride2", "conv_pipeline", "05_n2_c4_w20_oc15_s2",
    reference_profile="05_n2_c4_w20_oc15_s2",
    expect_conflicts=True,
)
verify_streaming(
    "c63-maxk", "conv_pipeline", "06_c63_w1_oc16_maxk",
    reference_profile="06_c63_w1_oc16_maxk",
)
verify_streaming(
    "w6-stride2-scattered", "streaming_conv_pipeline",
    "w6_stride2_scattered",
    expect_conflicts=True,
    expect_scattered=True,
    expect_input_bubbles=True,
)
verify_streaming(
    "n2-w6-depth1-no-reuse", "streaming_conv_pipeline",
    "n2_w6_stride2_depth1_no_reuse",
    expect_conflicts=True,
    expect_scattered=True,
    expect_input_bubbles=True,
    expect_b_refill=True,
    expect_depth_one_baseline=True,
)
verify_streaming(
    "n2-w6-depth2-no-reuse", "streaming_conv_pipeline",
    "n2_w6_stride2_depth2_no_reuse",
    expect_conflicts=True,
    expect_scattered=True,
    expect_input_bubbles=True,
    expect_b_refill=True,
)
verify_streaming(
    "n2-w6-full-reuse", "streaming_conv_pipeline",
    "n2_w6_stride2_full_reuse",
    expect_conflicts=True,
    expect_scattered=True,
    expect_weight_reuse=True,
)
verify_streaming(
    "n2-w16-depth1-ab-conflict", "streaming_conv_pipeline",
    "n2_w16_stride1_depth1_ab_conflict",
    expect_input_bubbles=True,
    expect_b_refill=True,
    expect_depth_one_baseline=True,
    expect_full_ab_conflict=True,
)
verify_streaming(
    "w32-stride2-conflict", "streaming_conv_pipeline",
    "w32_stride2_conflict",
    expect_conflicts=True,
)
verify_streaming(
    "output-backpressure", "conv_pipeline", "07_c2_w5_oc3_outbp",
    reference_profile="07_c2_w5_oc3_outbp",
    ready_period=11,
    ready_high=1,
    expect_output_backpressure=True,
)
verify_streaming(
    "target-w32", "conv_pipeline", "08_n1_c16_h16_w32_oc16",
    expect_full_exchange=True,
    expect_weight_reuse=True,
)
