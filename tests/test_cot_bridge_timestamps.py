"""Every line the bridge prints carries the time it was printed.

Without it, whether a delivered chat line arrived before or after the far end
learned its sender -- the whole question on the bench 2026-09-21 -- could only
be inferred from line order.
"""

import io
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import TimestampedStream  # noqa: E402

STAMP = re.compile(r"^\d\d:\d\d:\d\d ")


class TimestampTests(unittest.TestCase):
    def setUp(self):
        self.buffer = io.StringIO()
        self.stream = TimestampedStream(self.buffer)

    def lines(self):
        return self.buffer.getvalue().splitlines()

    def test_each_printed_line_is_stamped(self):
        print("[bridge] one", file=self.stream)
        print("[bridge] two\n[bridge] three", file=self.stream)
        self.assertEqual(len(self.lines()), 3)
        self.assertTrue(all(STAMP.match(line) for line in self.lines()))

    def test_a_line_written_in_pieces_is_stamped_once(self):
        self.stream.write("partial ")
        self.stream.write("line\n")
        self.assertEqual(len(self.lines()), 1)
        self.assertEqual(STAMP.sub("", self.lines()[0]), "partial line")


if __name__ == "__main__":
    unittest.main()
