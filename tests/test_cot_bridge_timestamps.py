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

    def test_threads_printing_at_once_never_share_a_line(self):
        """print() writes the message and the newline separately; with a shared
        flag another thread's text landed between them, unstamped."""
        import threading
        import time

        # The race is real but rare under CPython's default scheduling, so the
        # window is widened: switch threads as often as possible, and make the
        # underlying write yield. Under these conditions the previous wrapper
        # mangled 1,526 of 1,800 lines; without them it passed every time,
        # which would have made this test prove nothing.
        class YieldingBuffer(io.StringIO):
            def write(self, text):
                time.sleep(0)
                return super().write(text)

        self.buffer = YieldingBuffer()
        self.stream = TimestampedStream(self.buffer)
        previous = sys.getswitchinterval()
        sys.setswitchinterval(1e-6)
        try:
            def chatter(name):
                for index in range(300):
                    print("[%s] line %d" % (name, index), file=self.stream)

            workers = [threading.Thread(target=chatter, args=("t%d" % n,)) for n in range(6)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join()
        finally:
            sys.setswitchinterval(previous)

        lines = self.lines()
        self.assertEqual(len(lines), 6 * 300)
        self.assertTrue(all(re.match(r"^\d\d:\d\d:\d\d \[t\d\] line \d+$", line) for line in lines),
                        [line for line in lines if not re.match(r"^\d\d:\d\d:\d\d \[t\d\] line \d+$", line)][:3])


if __name__ == "__main__":
    unittest.main()
