#!/usr/bin/env python3
"""Unit tests for the SAU memory hex comparator."""

import io
import os
import tempfile
import unittest
from contextlib import redirect_stdout

from util.sau.compare_memory import (  # noqa: E402
    MemoryHexError,
    compare_bytes,
    main,
    read_byte_hex,
)


class ReadByteHexTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)

    def write(self, name, content):
        path = os.path.join(self.tempdir.name, name)
        with open(path, "w", encoding="utf-8") as hex_file:
            hex_file.write(content)
        return path

    def test_byte_per_line_ascending_addresses(self):
        path = self.write("bytes.hex", "0e\nff\n\nf4\n")
        self.assertEqual(read_byte_hex(path, 1), bytes([0x0E, 0xFF, 0xF4]))

    def test_sixteen_byte_words_are_little_endian(self):
        # First line of the atbd_cutbit8 package image: its last two hex
        # digits are the lowest addressed byte.
        line = "f9cef7f21bed11e8e60ce000e9e829fa"
        path = self.write("words.hex", line + "\n")
        data = read_byte_hex(path, 16)
        self.assertEqual(data[0], 0xFA)
        self.assertEqual(data[1], 0x29)
        self.assertEqual(data[15], 0xF9)

    def test_rejects_directives_and_bad_lines(self):
        for content in ("@1000\n", "// note\n", "abc\n", "zz\n"):
            path = self.write("bad.hex", content)
            with self.assertRaises(MemoryHexError):
                read_byte_hex(path, 1)


class CompareBytesTest(unittest.TestCase):
    def test_match_returns_no_mismatch(self):
        first, count = compare_bytes(b"\x01\x02", b"\x01\x02", 0x1000)
        self.assertIsNone(first)
        self.assertEqual(count, 0)

    def test_reports_first_mismatch_with_beat_and_lane(self):
        expected = bytearray(96)
        actual = bytearray(96)
        actual[69] = 0x33
        actual[70] = 0x44
        first, count = compare_bytes(
            bytes(expected), bytes(actual), 0x29120C00
        )
        self.assertEqual(count, 2)
        self.assertEqual(first["address"], 0x29120C00 + 69)
        self.assertEqual(first["expected"], 0x00)
        self.assertEqual(first["actual"], 0x33)
        self.assertEqual(first["beat"], 2)
        self.assertEqual(first["lane"], 5)


class MainTest(unittest.TestCase):
    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.addCleanup(self.tempdir.cleanup)

    def write(self, name, content):
        path = os.path.join(self.tempdir.name, name)
        with open(path, "w", encoding="utf-8") as hex_file:
            hex_file.write(content)
        return path

    def test_identical_files_exit_zero_silently(self):
        expected = self.write("expected.hex", "0e\nff\n")
        actual = self.write("actual.hex", "0e\nff\n")
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            status = main([expected, actual])
        self.assertEqual(status, 0)
        self.assertEqual(stdout.getvalue(), "")

    def test_mismatch_prints_address_beat_lane(self):
        expected = self.write("expected.hex", "0e\nff\n")
        actual = self.write("actual.hex", "0e\n00\n")
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            status = main([expected, actual, "--base", "0x29120c00"])
        self.assertEqual(status, 1)
        self.assertIn("0x29120c01", stdout.getvalue())
        self.assertIn("beat 0, lane 1", stdout.getvalue())

    def test_word_width_mix_compares_image_against_dump(self):
        line = "f9cef7f21bed11e8e60ce000e9e829fa"
        expected = self.write("image.hex", line + "\n")
        dump_lines = "".join(
            f"{byte:02x}\n" for byte in reversed(bytes.fromhex(line))
        )
        actual = self.write("dump.hex", dump_lines)
        status = main(
            [expected, actual, "--expected-word-bytes", "16"]
        )
        self.assertEqual(status, 0)

    def test_size_limits_the_comparison(self):
        expected = self.write("expected.hex", "0e\nff\n")
        actual = self.write("actual.hex", "0e\nff\n11\n")
        self.assertEqual(
            main([expected, actual, "--size", "2"]), 0
        )
        stdout = io.StringIO()
        with redirect_stdout(stdout):
            self.assertEqual(main([expected, actual]), 1)


if __name__ == "__main__":
    unittest.main()
