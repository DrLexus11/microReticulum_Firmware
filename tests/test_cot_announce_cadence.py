"""What each announce costs, and how often it is worth paying.

The node announce and the LXMF inbox announce were tied together when the inbox
one was added. They are needed for different things and expire on different
clocks: membership expires, so the node announce keeps a team knowing who is in
it, while an inbox announce only has to leave a path behind -- and an RNS path
lasts a week.

Tying them doubled announce airtime, and the greeting path amplified it to
about one inbox announce every fifteen seconds. On a LoRa channel already at 7%
occupancy, that is airtime taken from the link it exists to enable.
"""

import sys
import time
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_bridge                     # noqa: E402
from cot_bridge import CotBridge      # noqa: E402


def bridge(with_lxmf=True):
    made = CotBridge.__new__(CotBridge)
    made.team = "Cyan"
    made.secret = b"x" * 32
    made.callsign = "DECK"
    made.role = "Team Member"
    made.node = Mock()
    made._last_inbox_announce = 0.0
    made.lxmf = Mock() if with_lxmf else None
    return made


class InboxCadenceTests(unittest.TestCase):
    def test_the_first_announce_carries_the_inbox(self):
        """A node that has just started has left no path behind it."""
        made = bridge()
        made.announce()
        made.node.announce.assert_called_once()
        made.lxmf.announce.assert_called_once()

    def test_a_second_announce_soon_after_does_not(self):
        """The node announce still goes -- membership needs it -- and the
        inbox one does not, because the path it left is still there."""
        made = bridge()
        made.announce()
        made.announce()
        self.assertEqual(made.node.announce.call_count, 2)
        self.assertEqual(made.lxmf.announce.call_count, 1)

    def test_the_inbox_is_announced_again_once_its_interval_passes(self):
        made = bridge()
        made.announce()
        made._last_inbox_announce -= cot_bridge.INBOX_ANNOUNCE_INTERVAL_SECONDS + 1
        made.announce()
        self.assertEqual(made.lxmf.announce.call_count, 2)

    def test_the_greeting_storm_cannot_drag_the_inbox_with_it(self):
        """The case measured on hardware: greeting fires on every member
        announce, and with the two cadences tied that put an inbox announce on
        the air roughly every fifteen seconds."""
        made = bridge()
        for _ in range(40):
            made.announce()
        self.assertEqual(made.node.announce.call_count, 40)
        self.assertEqual(made.lxmf.announce.call_count, 1)

    def test_with_lxmf_disabled_nothing_extra_is_announced(self):
        made = bridge(with_lxmf=False)
        made.announce()
        made.node.announce.assert_called_once()

    def test_the_interval_is_far_longer_than_the_node_cadence(self):
        """Stated as a test because the two being close is the bug."""
        self.assertGreaterEqual(
            cot_bridge.INBOX_ANNOUNCE_INTERVAL_SECONDS,
            cot_bridge.ANNOUNCE_INTERVAL_SECONDS)


class ColdStartTests(unittest.TestCase):
    """A peer that has just appeared cannot wait half an hour for our inbox.

    The thirty-minute idle cadence was reasoned from "an RNS path lasts a
    week", which holds only for a peer that *heard* the announce. A node away
    for the whole window comes back with a path to our TAK node and none to our
    inbox, and those carry different things: markers and positions ride the
    node destination and work immediately, chat rides the inbox and has nowhere
    to go. Reported from the field 2026-09-20 as "markers always arrive,
    messaging needs a cold start period".
    """

    def test_a_greeting_carries_the_inbox_even_inside_the_idle_interval(self):
        made = bridge()
        made.announce()                       # idle announce, inbox goes
        made._last_inbox_announce -= cot_bridge.INBOX_ANNOUNCE_FLOOR_SECONDS + 1
        made.announce(greeting=True)
        self.assertEqual(made.lxmf.announce.call_count, 2)

    def test_the_greeting_floor_still_bounds_a_storm(self):
        """Ten nodes powering up together must not mean ten inbox announces."""
        made = bridge()
        made.announce(greeting=True)
        for _ in range(10):
            made.announce(greeting=True)
        self.assertEqual(made.lxmf.announce.call_count, 1)

    def test_an_idle_announce_still_waits_the_long_interval(self):
        """The airtime saving is kept. Only a peer appearing shortens it."""
        made = bridge()
        made.announce()
        made._last_inbox_announce -= cot_bridge.INBOX_ANNOUNCE_FLOOR_SECONDS + 1
        made.announce()
        self.assertEqual(made.lxmf.announce.call_count, 1)

    def test_the_greeting_floor_is_far_shorter_than_the_idle_interval(self):
        """Stated as a test because the two being equal is the bug."""
        self.assertLess(cot_bridge.INBOX_ANNOUNCE_FLOOR_SECONDS,
                        cot_bridge.INBOX_ANNOUNCE_INTERVAL_SECONDS)


if __name__ == "__main__":
    unittest.main()
