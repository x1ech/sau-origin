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
