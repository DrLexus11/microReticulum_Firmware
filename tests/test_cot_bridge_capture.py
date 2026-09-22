"""What ATAK sends is kept verbatim when asked, and only then.

D2 is designed against a capture of ATAK's own data package and QuickPic
events; the codecs reshape everything they handle, so the capture is taken
before any of them.
"""

import os
import stat
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge      # noqa: E402

EVENT = '<event version="2.0" uid="x" type="b-f-t-r"><point lat="1" lon="2"/></event>'


class CaptureTests(unittest.TestCase):
    def test_off_by_default(self):
        self.assertIsNone(CotBridge.capture_path)

    def test_an_event_is_appended_verbatim_and_owner_only(self):
        with tempfile.TemporaryDirectory() as folder:
            made = CotBridge.__new__(CotBridge)
            made.capture_path = os.path.join(folder, "atak.cot")
            made._capture(EVENT)
            made._capture(EVENT + "\n")
            with open(made.capture_path, encoding="utf-8") as kept:
                self.assertEqual(kept.read(), EVENT + "\n" + EVENT + "\n")
            self.assertEqual(stat.S_IMODE(os.stat(made.capture_path).st_mode), 0o600)

    def test_bytes_as_they_arrive_from_atak_are_kept(self):
        """The bridge hands _from_atak bytes. Assuming text killed ATAK's
        connection on every event, on the bench 2026-09-22."""
        with tempfile.TemporaryDirectory() as folder:
            made = CotBridge.__new__(CotBridge)
            made.capture_path = os.path.join(folder, "atak.cot")
            made._capture(EVENT.encode("utf-8"))
            with open(made.capture_path, "rb") as kept:
                self.assertEqual(kept.read(), EVENT.encode("utf-8") + b"\n")

    def test_a_capture_that_fails_does_not_raise(self):
        made = CotBridge.__new__(CotBridge)
        made.capture_path = "/nonexistent-directory/atak.cot"
        made._capture(EVENT.encode("utf-8"))


if __name__ == "__main__":
    unittest.main()
