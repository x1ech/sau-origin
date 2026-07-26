"""Unit tests for the SAU boundary-trace comparator."""

import contextlib
import io
import os
import tempfile
import unittest

from util.sau import compare_boundary

CLOCK = 1667
ANCHOR = 10 * CLOCK


def model_csv(rows):
    lines = ["signal,cycle,value"]
    for signal, cycle, value in rows:
        lines.append(f"{signal},{cycle},{value}")
    return "\n".join(lines) + "\n"


class CompareBoundaryTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)

    def write(self, name, content):
        path = os.path.join(self.directory.name, name)
        with open(path, "w", encoding="utf-8") as output:
            output.write(content)
        return path

    def run_main(self, argv):
        stdout = io.StringIO()
        stderr = io.StringIO()
        with contextlib.redirect_stdout(stdout), \
                contextlib.redirect_stderr(stderr):
            status = compare_boundary.main(argv)
        return status, stdout.getvalue(), stderr.getvalue()

    def anchored_golden(self, rows):
        # Mirror the real NPI export structure: a time-0 initial value
        # for every signal, the anchor rise, then the listed changes.
        lines = ["signal,time,value", "top.dut.SAU_1_inst.start,0,0"]
        for signal in sorted({signal for signal, _, _ in rows}):
            lines.append(f"top.dut.SAU_1_inst.{signal},0,0")
        lines.append(f"top.dut.SAU_1_inst.start,{ANCHOR},1")
        for signal, cycle, value in rows:
            lines.append(f"top.dut.SAU_1_inst.{signal},"
                         f"{ANCHOR + cycle * CLOCK},{value}")
        return self.write("golden.csv", "\n".join(lines) + "\n")

    def test_matching_sequences_pass_with_mixed_bases(self):
        golden = self.anchored_golden([
            ("data_B_valid", 3, "0"),
            ("data_B", 5, "0000101010"),
            ("data_B_valid", 5, "1"),
            ("data_B", 6, "0000000001"),
        ])
        actual = self.write("actual.csv", model_csv([
            ("data_B_valid", 0, "0"),
            ("data_B", 0, "0x0"),
            ("data_B", 5, "0x2a"),
            ("data_B_valid", 5, "1"),
            ("data_B", 6, "1"),
        ]))
        status, stdout, _ = self.run_main([golden, actual])
        self.assertEqual(status, 0)
        self.assertEqual(stdout, "")

    def test_value_mismatch_reports_signal_and_index(self):
        golden = self.anchored_golden([
            ("data_B", 5, "1010"),
            ("data_B", 6, "1100"),
        ])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0"),
            ("data_B", 5, "0xa"),
            ("data_B", 6, "0xd"),
        ]))
        status, stdout, _ = self.run_main([golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("data_B: change 2 differs", stdout)
        self.assertIn("expected 0xc", stdout)
        self.assertIn("actual 0xd", stdout)

    def test_cycles_mode_catches_a_shift_sequence_mode_accepts(self):
        golden = self.anchored_golden([("data_B", 5, "111")])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0"),
            ("data_B", 7, "0x7"),
        ]))
        status, _, _ = self.run_main([golden, actual])
        self.assertEqual(status, 0)
        status, stdout, _ = self.run_main(["--mode", "cycles",
                                          golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("at cycle 5", stdout)
        self.assertIn("at cycle 7", stdout)

    def test_pre_anchor_changes_fold_into_the_initial_state(self):
        # Values before and at the anchor edge collapse to one cycle-0
        # state; the anchor signal itself reads 1 at cycle 0.
        golden = self.anchored_golden([("data_B", 4, "1")])
        status, stdout, _ = self.run_main(["--inspect", golden])
        self.assertEqual(status, 0)
        self.assertIn("start: 1 changes", stdout)

    def test_duplicate_values_collapse_on_both_sides(self):
        golden = self.anchored_golden([
            ("data_B", 3, "101"),
            ("data_B", 9, "101"),
        ])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0"),
            ("data_B", 3, "0x5"),
            ("data_B", 4, "0x5"),
        ]))
        status, _, _ = self.run_main([golden, actual])
        self.assertEqual(status, 0)

    def test_missing_required_signal_is_an_error(self):
        golden = self.anchored_golden([("data_B", 3, "1")])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "1"),
        ]))
        status, _, stderr = self.run_main(
            ["--signals", "data_B,trans0_inRow", golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("trans0_inRow", stderr)

    def test_off_edge_golden_time_is_an_error(self):
        text = ("signal,time,value\n"
                "top.dut.SAU_1_inst.start,0,0\n"
                f"top.dut.SAU_1_inst.start,{ANCHOR},1\n"
                f"top.dut.SAU_1_inst.data_B,{ANCHOR + CLOCK + 3},1\n")
        golden = self.write("golden.csv", text)
        actual = self.write("actual.csv", model_csv([("data_B", 0, "1")]))
        status, _, stderr = self.run_main([golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("off a clock edge", stderr)

    def test_decreasing_model_cycle_is_an_error(self):
        golden = self.anchored_golden([("data_B", 3, "1")])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 5, "1"),
            ("data_B", 4, "0"),
        ]))
        status, _, stderr = self.run_main([golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("decreases", stderr)

    def test_qualified_signals_compare_accepted_values_only(self):
        # Wire-style golden signals drop to zero outside valid windows;
        # qualification keeps only the accepted payload values.
        golden = self.anchored_golden([
            ("data_B_valid", 5, "1"),
            ("data_B", 5, "1010"),
            ("data_B_valid", 6, "0"),
            ("data_B", 6, "0"),
            ("data_B_valid", 9, "1"),
            ("data_B", 9, "1100"),
            ("data_B", 10, "1111"),
            ("data_B_valid", 11, "0"),
            ("data_B", 11, "0"),
        ])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0xa"),
            ("data_B", 1, "0xc"),
            ("data_B", 2, "0xf"),
        ]))
        status, stdout, _ = self.run_main(
            ["--qualify", "data_B=data_B_valid",
             "--signals", "data_B", golden, actual])
        self.assertEqual(status, 0)
        self.assertEqual(stdout, "")
        # Without qualification the idle zeros make the sequences differ.
        status, _, _ = self.run_main(["--signals", "data_B",
                                      golden, actual])
        self.assertEqual(status, 1)

    def test_allow_actual_extra_accepts_a_truncated_golden_window(self):
        golden = self.anchored_golden([("data_B", 3, "1")])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0"),
            ("data_B", 3, "1"),
            ("data_B", 4, "0x2"),
        ]))
        status, _, _ = self.run_main([golden, actual])
        self.assertEqual(status, 1)
        status, _, _ = self.run_main(["--allow-actual-extra",
                                      golden, actual])
        self.assertEqual(status, 0)

    def test_qualify_with_missing_signal_is_an_error(self):
        golden = self.anchored_golden([("data_B", 3, "1")])
        actual = self.write("actual.csv", model_csv([("data_B", 0, "1")]))
        status, _, stderr = self.run_main(
            ["--qualify", "data_B=nope", golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("missing", stderr)

    def test_change_count_difference_is_reported(self):
        golden = self.anchored_golden([
            ("data_B", 3, "1"),
            ("data_B", 4, "10"),
        ])
        actual = self.write("actual.csv", model_csv([
            ("data_B", 0, "0"),
            ("data_B", 3, "1"),
        ]))
        status, stdout, _ = self.run_main([golden, actual])
        self.assertEqual(status, 1)
        self.assertIn("change count differs: expected 3, actual 2", stdout)


if __name__ == "__main__":
    unittest.main()
