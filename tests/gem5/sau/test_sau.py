import csv
import json
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


class VerifySauCausalBackpressure(verifier.Verifier):
    """Check fixture causality, token conservation, and memory pressure."""

    stat_pattern = re.compile(
        r"^system\.sau\.(\S+)\s+([0-9.eE+-]+)\s+#"
    )

    def __init__(self, rtl_profile, require_pressure=True):
        super().__init__()
        self.rtl_profile = rtl_profile
        self.require_pressure = require_pressure

    @staticmethod
    def architecture_counts(path):
        counts = {}
        with open(path, encoding="utf-8") as trace:
            next(trace)
            for row in trace:
                event = row.split(",", 2)[1]
                counts[event] = counts.get(event, 0) + 1
        return counts

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        expected_trace = joinpath(self.rtl_profile, "architecture.csv")
        actual_trace = joinpath(tempdir, "sau.csv")
        comparator = joinpath(
            config.base_dir, "util", "sau", "compare_trace.py"
        )
        result = subprocess.run(
            (
                sys.executable,
                comparator,
                "--mode",
                "causal",
                expected_trace,
                actual_trace,
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
            test_util.fail(
                "SAU causal timing-memory comparison failed for "
                f"{os.path.basename(self.rtl_profile)}:\n{output}\n"
                f"See {tempdir} for full results"
            )

        stats = {}
        with open(joinpath(tempdir, "stats.txt"), encoding="utf-8") as stream:
            for line in stream:
                match = self.stat_pattern.match(line)
                if match:
                    stats[match.group(1)] = float(match.group(2))

        expected = self.architecture_counts(expected_trace)
        required = {
            "commandsCompleted": expected.get("command_complete", 0),
            "readRequests": expected.get("read_accepted", 0),
            "writeRequests": expected.get("write_accepted", 0),
        }
        errors = []
        for name, value in required.items():
            if stats.get(name) != value:
                errors.append(
                    f"{name}: expected {value}, got {stats.get(name)}"
                )

        pressure_stats = (
            "stallRequestRetry",
            "stallOutstandingReadLimit",
            "stallOutstandingWriteLimit",
            "stallInputStarvation",
        )
        if self.require_pressure:
            if not any(stats.get(name, 0) > 0 for name in pressure_stats):
                errors.append(
                    "timing-memory run recorded no backpressure stalls"
                )
            if stats.get("maxOutstandingReadCount", 0) > 2:
                errors.append(
                    "maximum outstanding reads exceeded configured 2"
                )
            if stats.get("maxOutstandingWriteCount", 0) > 2:
                errors.append(
                    "maximum outstanding writes exceeded configured 2"
                )

        if errors:
            test_util.fail(
                "SAU causal/backpressure verification failed for "
                f"{os.path.basename(self.rtl_profile)}:\n" +
                "\n".join(errors) +
                f"\nSee {tempdir} for full results"
            )


class VerifySauFunctionalMemory(verifier.Verifier):
    """Compare final memory and any packaged strict timing contract."""

    stat_pattern = re.compile(
        r"^system\.sau\.(payloadReadBeats|payloadWriteBeats)"
        r"\s+([0-9.eE+-]+)\s+#"
    )

    def __init__(self, rtl_profile):
        super().__init__()
        self.rtl_profile = rtl_profile

    def test(self, params):
        tempdir = params.fixtures[constants.tempdir_fixture_name].path
        comparator = joinpath(
            config.base_dir, "util", "sau", "compare_memory.py"
        )
        result = subprocess.run(
            (
                sys.executable,
                comparator,
                joinpath(self.rtl_profile, "final_output_memory.hex"),
                joinpath(tempdir, "final_output_memory.hex"),
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
            test_util.fail(
                "SAU final-memory comparison failed for "
                f"{os.path.basename(self.rtl_profile)}:\n{output}\n"
                f"See {tempdir} for full results"
            )

        with open(
            joinpath(self.rtl_profile, "manifest.json"), encoding="utf-8"
        ) as stream:
            manifest = json.load(stream)
        timing = manifest.get("timing_contract")
        dependencies = manifest.get("memory_dependencies", ())

        commands = {}
        read_responses = 0
        with open(joinpath(tempdir, "sau.csv"), encoding="utf-8") as trace:
            for row in csv.DictReader(trace):
                command_id = int(row["command_id"])
                command = commands.setdefault(
                    command_id,
                    {
                        "result_beats": 0,
                        "write_beats": 0,
                        "read_events": {},
                        "write_events": [],
                    },
                )
                event = row["event"]
                if event == "command_accepted":
                    command["accepted"] = int(row["cycle"])
                elif event == "command_complete":
                    command["complete"] = int(row["cycle"])
                elif event == "result_produced":
                    command["result_beats"] += 1
                elif event == "write_accepted":
                    command["write_beats"] += 1
                    command["write_events"].append(
                        (int(row["cycle"]), int(row["address"], 0))
                    )
                elif event == "read_accepted":
                    command["read_events"].setdefault(
                        row["stream"], []
                    ).append(
                        (int(row["cycle"]), int(row["address"], 0))
                    )
                elif event == "read_response_visible":
                    read_responses += 1

        ordered = [commands[key] for key in sorted(commands)]
        actual = {
            "command_extent_cycles": [
                command["complete"] - command["accepted"]
                for command in ordered
            ],
            "inter_command_gap_cycles": [
                ordered[index + 1]["accepted"] - command["complete"]
                for index, command in enumerate(ordered[:-1])
            ],
            "result_beats_per_command": [
                command["result_beats"] for command in ordered
            ],
            "write_beats_per_command": [
                command["write_beats"] for command in ordered
            ],
        }
        errors = [] if not timing else [
            f"{field}: expected {expected}, got {actual.get(field)}"
            for field, expected in timing.items()
            if actual.get(field) != expected
        ]
        if errors:
            test_util.fail(
                "SAU strict functional timing comparison failed for "
                f"{os.path.basename(self.rtl_profile)}:\n" +
                "\n".join(errors) +
                f"\nSee {tempdir} for full results"
            )

        dependency_errors = []
        for dependency in dependencies:
            beat_bytes = manifest["beat_bytes"]
            producer_id = dependency["producer_command"]
            consumer_id = dependency["consumer_command"]
            stream = dependency["stream"]
            base = int(dependency["base"], 0)
            size_bytes = dependency["size_bytes"]
            beat_count = dependency["beat_count"]
            if beat_count * beat_bytes != size_bytes:
                dependency_errors.append(
                    f"commands {producer_id}->{consumer_id}: "
                    "beat_count * beat_bytes does not equal size_bytes"
                )
                continue
            expected_addresses = [
                base + beat * beat_bytes for beat in range(beat_count)
            ]
            producer = commands.get(producer_id, {})
            consumer = commands.get(consumer_id, {})
            write_events = producer.get("write_events", [])
            read_events = consumer.get("read_events", {}).get(stream, [])
            write_addresses = [address for _, address in write_events]
            read_addresses = [address for _, address in read_events]
            if write_addresses != expected_addresses:
                dependency_errors.append(
                    f"command {producer_id} writes: expected "
                    f"{[hex(address) for address in expected_addresses]}, "
                    f"got {[hex(address) for address in write_addresses]}"
                )
            if read_addresses != expected_addresses:
                dependency_errors.append(
                    f"command {consumer_id} {stream} reads: expected "
                    f"{[hex(address) for address in expected_addresses]}, "
                    f"got {[hex(address) for address in read_addresses]}"
                )
            if (
                write_events
                and read_events
                and write_events[-1][0] >= read_events[0][0]
            ):
                dependency_errors.append(
                    f"commands {producer_id}->{consumer_id}: final write "
                    f"cycle {write_events[-1][0]} is not before first read "
                    f"cycle {read_events[0][0]}"
                )
        if dependency_errors:
            test_util.fail(
                "SAU command memory-dependency verification failed for "
                f"{os.path.basename(self.rtl_profile)}:\n" +
                "\n".join(dependency_errors) +
                f"\nSee {tempdir} for full results"
            )

        stats = {}
        with open(joinpath(tempdir, "stats.txt"), encoding="utf-8") as stream:
            for line in stream:
                match = self.stat_pattern.match(line)
                if match:
                    stats[match.group(1)] = int(float(match.group(2)))
        expected_stats = {
            "payloadReadBeats": read_responses,
            "payloadWriteBeats": sum(
                command["write_beats"] for command in ordered
            ),
        }
        stat_errors = [
            f"{name}: expected {expected}, got {stats.get(name)}"
            for name, expected in expected_stats.items()
            if stats.get(name) != expected
        ]
        if stat_errors:
            test_util.fail(
                "SAU strict payload statistics failed for "
                f"{os.path.basename(self.rtl_profile)}:\n" +
                "\n".join(stat_errors) +
                f"\nSee {tempdir} for full results"
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


def verify_sau_rtl_timing_memory(name):
    rtl_profile = joinpath(
        config.base_dir, "tests", "gem5", "sau", "ref", name
    )
    gem5_verify_config(
        name=f"sau-causal-{name}",
        fixtures=(),
        verifiers=(
            verifier.MatchRegex(exit_regex),
            VerifySauCausalBackpressure(rtl_profile),
        ),
        config=joinpath(
            config.base_dir, "configs", "example", "sau_timing.py"
        ),
        config_args=[
            f"--rtl-profile={rtl_profile}",
            "--timing-memory",
            "--fixture-start-policy=sequential",
            "--memory-latency=20ns",
            "--memory-latency-var=5ns",
            "--memory-bandwidth=1GiB/s",
            "--max-outstanding-reads=2",
            "--max-outstanding-writes=2",
        ],
        valid_isas=(constants.riscv_tag,),
        length=constants.quick_tag,
    )


for rtl_profile in (
    "int8_gemm_32x32x32_single_flow",
    "int8_gemm_64x32x256_k_sweep",
    "int8_gemm_64x256x32_n_sweep",
    "int8_gemm_32x256x256_m_sweep",
    "int8_gemm_64x256x256_baseline",
    "int8_gemm_96x256x256_m_holdout",
    "int8_gemm_64x128x256_k_holdout",
    "int8_gemm_64x256x128_n_holdout",
):
    verify_sau_rtl_profile(rtl_profile)
    verify_sau_rtl_timing_memory(rtl_profile)


flow1_functional_profile = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "sau",
    "functional_ref",
    "int8_gemm_64x160x64_atbd_flow1_cutbit8",
)
gem5_verify_config(
    name="sau-functional-int8_gemm_64x160x64_atbd_flow1_cutbit8",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(exit_regex),
        VerifySauFunctionalMemory(flow1_functional_profile),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "sau",
        "configs",
        "sau_flow1_functional.py",
    ),
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    length=constants.quick_tag,
)

flow2_functional_profile = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "sau",
    "functional_ref",
    "int8_gemm_32x512x32_atbd_flow2_cutbit8",
)
gem5_verify_config(
    name="sau-functional-int8_gemm_32x512x32_atbd_flow2_cutbit8",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(exit_regex),
        VerifySauFunctionalMemory(flow2_functional_profile),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "sau",
        "configs",
        "sau_flow2_functional.py",
    ),
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    length=constants.quick_tag,
)

flow2_double_functional_profile = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "sau",
    "functional_ref",
    "int8_gemm_32x768x32_atbd_flow2_cutbit8",
)
gem5_verify_config(
    name="sau-functional-int8_gemm_32x768x32_atbd_flow2_cutbit8",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(exit_regex),
        VerifySauFunctionalMemory(flow2_double_functional_profile),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "sau",
        "configs",
        "sau_flow2_double_functional.py",
    ),
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    length=constants.quick_tag,
)

chain_functional_profile = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "sau",
    "functional_ref",
    "int8_gemm_chain_32x768x32_to_32x32x32_atbd_cutbit8",
)
gem5_verify_config(
    name=(
        "sau-functional-"
        "int8_gemm_chain_32x768x32_to_32x32x32_atbd_cutbit8"
    ),
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(exit_regex),
        VerifySauFunctionalMemory(chain_functional_profile),
    ),
    config=joinpath(
        config.base_dir,
        "tests",
        "gem5",
        "sau",
        "configs",
        "sau_chain_functional.py",
    ),
    config_args=(),
    valid_isas=(constants.riscv_tag,),
    length=constants.quick_tag,
)


legacy_direct_profile = joinpath(
    config.base_dir,
    "tests",
    "gem5",
    "sau",
    "ref",
    "int8_gemm_64x256x256_baseline",
)
gem5_verify_config(
    name="sau-legacy-direct-command",
    fixtures=(),
    verifiers=(
        verifier.MatchRegex(exit_regex),
        VerifySauCausalBackpressure(
            legacy_direct_profile, require_pressure=False
        ),
    ),
    config=joinpath(
        config.base_dir, "configs", "example", "sau_timing.py"
    ),
    config_args=[
        "--calibration-memory",
        "--calibration-read-latency-cycles=4",
        "--memory-size=1GiB",
        "--command-count=2",
        "--inter-command-gap-cycles=353",
        "--a-base=0x29120000",
        "--b-base=0x29124000",
        "--output-base=0x29138000",
        "--a-command-stride=0x2000",
        "--b-command-stride=0",
        "--output-command-stride=0x2000",
        "--a-flow-stride=0",
        "--b-flow-stride=0x20",
        "--b-stride-bytes=0x100",
        "--output-instruction-stride=0x2000",
        "--b-read-start-ahead-beats=24",
        "--command-start-cycles=3",
    ],
    valid_isas=(constants.riscv_tag,),
    length=constants.quick_tag,
)


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
