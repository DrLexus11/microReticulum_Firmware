"""A frame from a sender not yet known is held, not dropped.

Measured on the bench 2026-09-21: a Nexus 6P joined the team and its first
exchange lost a chat line -- delivered with a proof, dropped because the
receiver had not yet heard the sender announce.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_chat                          # noqa: E402
import cot_pending                       # noqa: E402
from cot_bridge import CotBridge, TeamAnnounceHandler  # noqa: E402

NEXUS = b"\x33" * 16


class PendingAttributionTests(unittest.TestCase):
    def setUp(self):
        self.pending = cot_pending.PendingAttribution(hold_seconds=600, max_held=3)

    def test_a_frame_is_released_once_its_sender_resolves(self):
        self.pending.hold(b"frame", NEXUS, now=0)
        self.assertEqual(self.pending.ready(lambda raw: False, now=10), [])
        self.assertEqual(self.pending.ready(lambda raw: True, now=20), [(b"frame", NEXUS)])
        self.assertEqual(len(self.pending), 0)

    def test_a_frame_nobody_claims_is_dropped_after_the_hold(self):
        """A sender who never turns out to be a teammate is somebody else."""
        self.pending.hold(b"frame", NEXUS, now=0)
        self.assertEqual(self.pending.ready(lambda raw: True, now=601), [])
        self.assertEqual(self.pending.expired, 1)

    def test_the_hold_is_bounded(self):
        for index in range(10):
            self.pending.hold(bytes([index]), None, now=index)
        self.assertEqual(len(self.pending), 3)


class BridgeHoldsUnknownSendersTests(unittest.TestCase):
    """The bench case, end to end through the bridge's own paths."""

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.unreadable = made.received = 0
        made.chat_received = 0
        made._unattributed = cot_pending.PendingAttribution()
        made.registry = Mock()
        made.known = False
        made.registry.resolve_sender_id.side_effect = lambda sender_id: NEXUS if made.known else None
        made.registry.describe.return_value = {"callsign": "NEXUS"}
        made.rns = Mock()
        made.drawn = []
        made._dispatch_frame = Mock(side_effect=lambda raw, signed_by=None: made.drawn.append(raw))
        return made

    def chat(self):
        return cot_chat.encode(cot_chat.KIND_MESSAGE, 0x33333333, "5d0a1b2c-3d4e-4f60-8a9b-0c1d2e3f4a5b", "NEXUS",
                               text="SALT", recipient="urtn-" + "aa" * 16)

    def test_a_line_from_a_sender_not_yet_known_is_drawn_once_they_announce(self):
        made = self.bridge()
        with redirect_stdout(io.StringIO()):
            self.assertTrue(made._chat_from_mesh(self.chat(), signed_by=NEXUS))
            self.assertEqual(made.drawn, [])

            made.known = True
            made._member_joined(NEXUS)

        self.assertEqual(made.drawn, [self.chat()])

    def test_the_sender_is_asked_for_its_announce_straight_away(self):
        made = self.bridge()
        with redirect_stdout(io.StringIO()):
            made._chat_from_mesh(self.chat(), signed_by=NEXUS)
        made.rns.Transport.request_path.assert_called_once_with(NEXUS)

    def test_a_bare_packet_names_nobody_to_ask_and_simply_waits(self):
        made = self.bridge()
        with redirect_stdout(io.StringIO()):
            made._chat_from_mesh(self.chat(), signed_by=None)
        made.rns.Transport.request_path.assert_not_called()
        self.assertEqual(len(made._unattributed), 1)

    def test_the_announce_handler_hears_path_responses(self):
        """Otherwise the announce the request brings back is never read."""
        self.assertTrue(TeamAnnounceHandler.receive_path_responses)


if __name__ == "__main__":
    unittest.main()
