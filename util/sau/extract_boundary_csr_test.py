#!/usr/bin/env python3

import unittest

from util.sau.extract_boundary_csr import extract


ZERO_PS = 1250
PERIOD_PS = 1667


def time_at(cycle):
    return ZERO_PS + cycle * PERIOD_PS


def binary(value, width=64):
    return format(value, "0{}b".format(width))


def stripped_events():
    data = (
        0x2912040029120000,
        0x0008004101000000,
        0x0040010108004100,
        0x0000000000140012,
        0x0101012008004100,
        0x0104180101010100,
        0x29120C0080180041,
    )
    events = {}
    for cycle, value in zip(
            (28982, 28998, 29003, 29006, 29009, 29012, 29015), data):
        events[time_at(cycle)] = [("csr_wdata", binary(value), 0)]
    events[time_at(29006) + PERIOD_PS // 2] = [
        ("trans_mode", binary(2, 2), 0),
        ("reuse_mode", binary(1, 2), 0),
        ("cutbit", binary(8, 5), 0),
    ]
    events[time_at(29016)] = [("start", "1", 0)]
    return events


class FixedWriteOrderInferenceTest(unittest.TestCase):
    def test_recovers_abtd_replay_and_snapshot(self):
        writes, snapshots = extract(
            stripped_events(), ZERO_PS, PERIOD_PS,
            infer_fixed_write_order=True)

        self.assertEqual(
            [row["csr_addr"] for row in writes],
            ["0x200", "0x202", "0x204", "0x206",
             "0x208", "0x20a", "0x20c"])
        self.assertEqual(
            [row["cycle"] for row in writes],
            [28983, 28999, 29004, 29007, 29010, 29013, 29016])
        self.assertEqual(snapshots[0]["start_write_cycle"], 29016)
        self.assertEqual(snapshots[0]["start_write_row"], 7)
        self.assertEqual(snapshots[0]["trans_mode"], "0x2")
        self.assertEqual(snapshots[0]["reuse_mode"], "0x1")
        self.assertEqual(snapshots[0]["cutbit"], 8)

    def test_rejects_missing_data_pulse(self):
        events = stripped_events()
        del events[time_at(29012)]

        with self.assertRaisesRegex(ValueError, "exactly seven"):
            extract(
                events, ZERO_PS, PERIOD_PS,
                infer_fixed_write_order=True)

    def test_rejects_nonadjacent_start(self):
        events = stripped_events()
        events[time_at(29017)] = events.pop(time_at(29016))

        with self.assertRaisesRegex(ValueError, "one cycle after"):
            extract(
                events, ZERO_PS, PERIOD_PS,
                infer_fixed_write_order=True)

    def test_rejects_mode_mismatch(self):
        events = stripped_events()
        mode_time = time_at(29006) + PERIOD_PS // 2
        events[mode_time][0] = ("trans_mode", binary(1, 2), 0)

        with self.assertRaisesRegex(ValueError, "differs from observed"):
            extract(
                events, ZERO_PS, PERIOD_PS,
                infer_fixed_write_order=True)


if __name__ == "__main__":
    unittest.main()
