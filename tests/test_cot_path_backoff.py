"""An absent member is not asked for with every outgoing position.

Overnight 2026-09-22 the bridge requested a path to a test peer that had left
105 times -- once per outgoing position, every three minutes, for the six
hours a member is kept. Each request is a broadcast that crosses LoRa.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge      # noqa: E402

GONE = b"\x35" * 16


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.unreachable = 0
    made.rns = Mock()
    made.rns.Identity.recall.return_value = "identity"
    made.rns.Transport.has_path.return_value = False
    return made


class PathBackoffTests(unittest.TestCase):
    def test_requests_back_off_while_a_member_stays_absent(self):
        made = bridge()
        asked = [t for t in range(0, 6 * 3600, 180) if made._ask_for_path(GONE, now=t)]
        # Floor 60 s doubling to a 30-minute cap: a handful in six hours, not 120.
        self.assertLessEqual(len(asked), 16)
        self.assertEqual(asked[0], 0, "the first send still asks at once")
        gaps = [b - a for a, b in zip(asked, asked[1:])]
        self.assertEqual(gaps, sorted(gaps), "the gaps only grow")
        self.assertLessEqual(max(gaps), CotBridge.PATH_REQUEST_CAP_SECONDS + 180)

    def test_hearing_the_member_resets_the_backoff(self):
        made = bridge()
        made._heard_mesh = made._save_members = made.collect_held = Mock()
        made.registry = Mock()
        made.registry.should_greet.return_value = False
        for t in range(0, 3600, 180):
            made._ask_for_path(GONE, now=t)
        made._member_heard(GONE, None)
        self.assertTrue(made._ask_for_path(GONE, now=3601))

    def test_a_send_with_no_path_is_still_refused_and_counted(self):
        made = bridge()
        with redirect_stdout(io.StringIO()):
            self.assertEqual(made._send_to(GONE, b"\x02frame"), 0)
            self.assertEqual(made._send_to(GONE, b"\x02frame"), 0)
        self.assertEqual(made.unreachable, 2)
        self.assertEqual(made.rns.Transport.request_path.call_count, 1)


if __name__ == "__main__":
    unittest.main()
