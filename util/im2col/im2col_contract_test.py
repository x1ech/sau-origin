#!/usr/bin/env python3
"""Unit tests for the frozen Im2Col configuration and cycle contracts."""

from dataclasses import replace
import json
import unittest

from util.im2col.im2col_contract import (
    ConfigError,
    ControlRegisters,
    Im2ColState,
    TRACE_FIELDS,
    UINT64_MAX,
    begin_next,
    canonical_config_bytes,
    checked_add,
    checked_multiply,
    commit,
    cycle_zero_registers,
    done_transition_next,
    observe,
    resolved_config_sha256,
    validate_and_derive,
    ResolvedConfig,
)


def valid_config(**changes):
    config = ResolvedConfig(
        name="w5_pack3_pad1_stride1",
        n=1,
        c=2,
        h=4,
        w=5,
        out_h=4,
        out_w=5,
        kernel_h=3,
        kernel_w=3,
        stride_h=1,
        stride_w=1,
        dilation_h=1,
        dilation_w=1,
        pad_top=1,
        pad_left=1,
        spad_base=0,
    )
    return replace(config, **changes)


class ResolvedConfigTest(unittest.TestCase):
    def test_derives_packed_w5_counts(self):
        derived = validate_and_derive(valid_config())

        self.assertEqual(3, derived.rows_per_word)
        self.assertEqual(1, derived.w_words)
        self.assertEqual(2, derived.spatial_words_per_channel)
        self.assertEqual(4, derived.total_spatial_words)
        self.assertEqual(2, derived.h_groups)
        self.assertEqual(1, derived.w_groups)
        self.assertEqual(36, derived.expected_vectors)

    def test_derives_split_w20_counts(self):
        config = valid_config(
            name="w20_split_pad1_stride1",
            h=3,
            w=20,
            out_h=3,
            out_w=20,
        )
        derived = validate_and_derive(config)

        self.assertEqual(1, derived.rows_per_word)
        self.assertEqual(2, derived.w_words)
        self.assertEqual(6, derived.spatial_words_per_channel)
        self.assertEqual(12, derived.total_spatial_words)
        self.assertEqual(3, derived.h_groups)
        self.assertEqual(2, derived.w_groups)
        self.assertEqual(108, derived.expected_vectors)

    def test_rejects_every_field_range(self):
        invalid_values = {
            "schema_version": 2,
            "n": 0,
            "c": 65536,
            "h": 0,
            "w": 65536,
            "out_h": 0,
            "out_w": 65536,
            "pad_top": 65536,
            "pad_left": -1,
            "kernel_h": 0,
            "kernel_w": 16,
            "stride_h": 0,
            "stride_w": 16,
            "dilation_h": 0,
            "dilation_w": 16,
            "spad_base": 4096,
            "cfg_dw_mode": 1,
            "cfg_kernel_pattern": 0xFFFE,
        }
        for field, value in invalid_values.items():
            with self.subTest(field=field), self.assertRaises(ConfigError):
                validate_and_derive(valid_config(**{field: value}))

    def test_rejects_non_integer_numeric_field(self):
        with self.assertRaisesRegex(ConfigError, "n must be an integer"):
            validate_and_derive(valid_config(n=True))

    def test_rejects_unsupported_generator_and_non_string_name(self):
        with self.assertRaisesRegex(ConfigError, "input_generator"):
            validate_and_derive(valid_config(input_generator="other"))
        with self.assertRaisesRegex(ConfigError, "name must be a string"):
            validate_and_derive(valid_config(name=7))

    def test_rejects_kernel_area_above_block_size(self):
        with self.assertRaisesRegex(ConfigError, "kernel_h \* kernel_w"):
            validate_and_derive(valid_config(kernel_h=5, kernel_w=4))

    def test_rejects_unsupported_packed_output_width(self):
        with self.assertRaisesRegex(ConfigError, "out_w must be <= w"):
            validate_and_derive(valid_config(out_w=6))

    def test_rejects_scratchpad_footprint_past_last_row(self):
        with self.assertRaisesRegex(ConfigError, "total_spatial_words"):
            validate_and_derive(valid_config(spad_base=4093))

    def test_accepts_scratchpad_footprint_exactly_at_boundary(self):
        derived = validate_and_derive(valid_config(spad_base=4092))
        self.assertEqual(4, derived.total_spatial_words)

    def test_checked_uint64_arithmetic_rejects_overflow(self):
        self.assertEqual(UINT64_MAX, checked_add(UINT64_MAX - 1, 1, "sum"))
        self.assertEqual(UINT64_MAX - 1,
                         checked_multiply((UINT64_MAX - 1) // 2, 2, "product"))
        with self.assertRaisesRegex(ConfigError, "sum overflows"):
            checked_add(UINT64_MAX, 1, "sum")
        with self.assertRaisesRegex(ConfigError, "product overflows"):
            checked_multiply(UINT64_MAX, 2, "product")

    def test_canonical_json_and_hash_are_deterministic(self):
        config = valid_config(name="宽度5")
        canonical = canonical_config_bytes(config)
        decoded = canonical.decode("utf-8")

        self.assertIn("宽度5", decoded)
        self.assertNotIn("\\u5bbd", decoded)
        self.assertNotIn(" ", decoded)
        self.assertEqual(json.loads(decoded)["name"], "宽度5")
        digest = resolved_config_sha256(config)
        self.assertEqual(64, len(digest))
        self.assertEqual(digest, resolved_config_sha256(config))


class CycleContractTest(unittest.TestCase):
    def test_freezes_state_encoding_and_trace_header(self):
        self.assertEqual(
            [0, 1, 2, 3, 4, 5],
            [state.value for state in Im2ColState],
        )
        self.assertEqual(47, len(TRACE_FIELDS))
        self.assertEqual("req_addr_b00", TRACE_FIELDS[10])
        self.assertEqual("req_addr_b15", TRACE_FIELDS[25])
        self.assertEqual("resp_data_b00", TRACE_FIELDS[27])
        self.assertEqual("resp_data_b15", TRACE_FIELDS[42])
        self.assertEqual(
            ("feed_valid", "feed_ready", "feed_data", "feed_mask"),
            TRACE_FIELDS[-4:],
        )

    def test_cycle_zero_anchor_is_issue_busy_without_done(self):
        observation = observe(0, cycle_zero_registers())

        self.assertEqual(Im2ColState.ISSUE, observation.state)
        self.assertTrue(observation.busy)
        self.assertFalse(observation.done)

    def test_done_transition_uses_old_next_commit_semantics(self):
        old = ControlRegisters(state=Im2ColState.DONE, done=False)
        old_observation = observe(9, old)
        next_registers = done_transition_next(old)

        self.assertEqual(Im2ColState.DONE, old_observation.state)
        self.assertTrue(old_observation.busy)
        self.assertFalse(old_observation.done)
        self.assertEqual(old, ControlRegisters(Im2ColState.DONE, False))

        committed = commit(old, next_registers)
        next_observation = observe(10, committed)
        self.assertEqual(Im2ColState.IDLE, next_observation.state)
        self.assertFalse(next_observation.busy)
        self.assertTrue(next_observation.done)

        after_default = begin_next(committed)
        self.assertFalse(after_default.done)

    def test_done_transition_rejects_other_old_states(self):
        with self.assertRaisesRegex(ValueError, "old state ST_DONE"):
            done_transition_next(ControlRegisters(Im2ColState.NEXT, False))


if __name__ == "__main__":
    unittest.main()
