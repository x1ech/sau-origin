#!/usr/bin/env python3
"""Compare two SAU memory hex files under the frozen byte-order contract.

Each non-blank line holds one little-endian word of ``word-bytes`` bytes:
the last two hex digits are the lowest addressed byte. The default word
width of 1 matches both the golden ``final_output_memory.hex`` files and
gem5 ``--final-memory-dump`` output; width 16 reads the RTL
``initial_memory.hex`` format. Strict and timing-memory dumps share one
writer, so this is the single workload-level comparator for both.

On a mismatch the first differing absolute address, the expected and
actual bytes, the containing 256-bit beat/lane (counted from the compare
base), and the total differing byte count are reported.
"""

import argparse
import sys

BEAT_BYTES = 32


class MemoryHexError(Exception):
    """Raised when a memory hex file violates the image contract."""


def read_byte_hex(path, word_bytes):
    """Read a little-endian word-per-line hex file into bytes."""
    if word_bytes <= 0:
        raise MemoryHexError("word width must be positive")
    data = bytearray()
    with open(path, encoding="utf-8") as hex_file:
        for number, line in enumerate(hex_file, start=1):
            text = line.strip()
            if not text:
                continue
            if text.startswith("@") or text.startswith("//"):
                raise MemoryHexError(
                    f"{path} line {number}: $readmemh directives and "
                    "comments are not part of the image contract"
                )
            if len(text) != 2 * word_bytes:
                raise MemoryHexError(
                    f"{path} line {number}: expected {2 * word_bytes} hex "
                    f"digits, found {len(text)}"
                )
            try:
                word = bytes.fromhex(text)
            except ValueError as error:
                raise MemoryHexError(
                    f"{path} line {number}: {error}"
                ) from error
            data.extend(reversed(word))
    return bytes(data)


def compare_bytes(expected, actual, base):
    """Return (first_mismatch, mismatch_count); first_mismatch is a dict."""
    first = None
    count = 0
    for offset, (want, have) in enumerate(zip(expected, actual)):
        if want == have:
            continue
        count += 1
        if first is None:
            first = {
                "address": base + offset,
                "expected": want,
                "actual": have,
                "beat": offset // BEAT_BYTES,
                "lane": offset % BEAT_BYTES,
            }
    return first, count


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("expected", help="golden memory hex file")
    parser.add_argument("actual", help="gem5 memory dump hex file")
    parser.add_argument(
        "--expected-word-bytes", type=int, default=1,
        help="little-endian word bytes per expected-file line",
    )
    parser.add_argument(
        "--actual-word-bytes", type=int, default=1,
        help="little-endian word bytes per actual-file line",
    )
    parser.add_argument(
        "--base", type=lambda value: int(value, 0), default=0,
        help="absolute address of byte 0, used for reporting",
    )
    parser.add_argument(
        "--size", type=lambda value: int(value, 0), default=0,
        help="compare only the first SIZE bytes of both files",
    )
    args = parser.parse_args(argv)

    try:
        expected = read_byte_hex(args.expected, args.expected_word_bytes)
        actual = read_byte_hex(args.actual, args.actual_word_bytes)
    except (OSError, MemoryHexError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    if args.size:
        if len(expected) < args.size or len(actual) < args.size:
            print(
                f"error: --size {args.size} exceeds file contents "
                f"({len(expected)} expected, {len(actual)} actual bytes)",
                file=sys.stderr,
            )
            return 1
        expected = expected[:args.size]
        actual = actual[:args.size]
    elif len(expected) != len(actual):
        print(
            f"error: byte counts differ: {len(expected)} expected, "
            f"{len(actual)} actual",
            file=sys.stderr,
        )
        return 1

    first, count = compare_bytes(expected, actual, args.base)
    if first is None:
        return 0
    print(
        f"first mismatch at {first['address']:#x}: "
        f"expected {first['expected']:#04x} actual {first['actual']:#04x} "
        f"(beat {first['beat']}, lane {first['lane']}); "
        f"{count} differing bytes total"
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
