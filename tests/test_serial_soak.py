"""serial_soak's summary: one count per boot, and [tables] apart from [diag]."""

import io
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

import serial_soak  # noqa: E402

ECHO = [
    "[boot] --- bootlog history (120 bytes) ---",
    "[boot] boot reason=TASK_WDT prev=9796s",
    "[boot] boot reason=POWERON prev=hw-reset",
    "[boot] boot reason=SW (ESP.restart) prev=37960s",
    "[boot] --- end bootlog ---",
]


def summary(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        for i, line in enumerate(lines):
            f.write("2026-09-29 10:%02d:%02d %s\n" % (i // 60, i % 60, line))
        path = f.name
    out = io.StringIO()
    serial_soak.summarise(path, out=out)
    Path(path).unlink()
    return out.getvalue()


class BootCounting(unittest.TestCase):

    def test_live_line_and_its_echo_are_one_boot(self):
        text = summary(["[boot] reset reason: SW (ESP.restart) (3), software reset, previous run 37960s"] + ECHO)
        self.assertIn("boots: 1", text)

    def test_an_echo_alone_counts_its_newest_entry_only(self):
        # A USB-CDC board prints the live line before the host reattaches.
        text = summary(ECHO)
        self.assertIn("boots: 1", text)
        self.assertIn("SW (ESP.restart)", text)
        self.assertNotIn("TASK_WDT", text.split("boots: 1")[1].split("\n")[1])

    def test_two_boots_with_their_echoes_are_two(self):
        live = "[boot] reset reason: SW (ESP.restart) (3), software reset, previous run 60s"
        lines = [live] + ECHO + ["[mem] internal=70000 largest=50000 psram=2000000"] * 200 + [live] + ECHO
        self.assertIn("boots: 2", summary(lines))


class TablesAndDiag(unittest.TestCase):

    def test_diag_rows_do_not_zero_the_transport_tables(self):
        text = summary([
            "[tables] newpaths=24 links=0 active=1 dests=7",
            "[diag] links=1 request_receipts=0 resources=0",
            "[tables] newpaths=27 links=0 active=1 dests=7",
            "[diag] links=2 request_receipts=1 resources=0",
        ])
        self.assertRegex(text, r"tables \(first -> last, max\):\n(.*\n)*\s+dests\s+7 ->\s+7")
        self.assertRegex(text, r"diag \(first -> last, max\):\n(.*\n)*\s+request_receipts\s+0 ->\s+1")


if __name__ == "__main__":
    unittest.main()
