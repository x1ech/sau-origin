import re

from testlib import (
    config,
    constants,
    gem5_verify_config,
    joinpath,
    verifier,
)


exit_regex = re.compile(r"SAU timing simulation exited: SAU command complete")


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
