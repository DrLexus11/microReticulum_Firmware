"""Who a chat line is drawn as, and what that is based on.

The frame carries a 32-bit sender_id. That is a lookup key, not a claim worth
trusting on its own: any member with a valid identity can put another member's
id in it. On a callout a line attributed to the command post carries the
command post's authority, so this matters inside an authenticated fleet as well
as outside one -- the threat is a compromised node, not a stranger.

LXMF proves who sent a message. That proof was being discarded.
"""

import collections
import io
import sys
import threading
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_chat                       # noqa: E402
from cot_bridge import CotBridge      # noqa: E402

ALICE = b"\xaa" * 16
MALLORY = b"\xbb" * 16


class Registry:
    """Every sender_id resolves to Alice, which is the point: the frame says
    so, and the frame is what an attacker controls."""

    def resolve_sender_id(self, _sender_id):
        return ALICE

    def describe(self, _hash):
        return {"callsign": "ALICE"}


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.rns = Mock()
    made.registry = Registry()
    made.team = "Cyan"
    made.clients = []
    made.clients_lock = threading.Lock()
    made._replay = collections.deque(maxlen=64)
    made._replay_lock = threading.Lock()
    made.replayed = 0
    made.chat_received = 0
    made.chat_misattributed = 0
    made.unreadable = 0
    made.received = 0
    made.drawn = []
    made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
    return made


def a_chat_frame():
    return cot_chat.encode(
        cot_chat.KIND_MESSAGE,
        0x01020304,
        "11111111-1111-1111-1111-111111111111",
        "All Chat Rooms",
        text="fall back to the north gate",
        sent_unix=1700000000,
    )


class AttributionTests(unittest.TestCase):
    def test_a_line_signed_by_the_member_it_names_is_drawn(self):
        made = bridge()
        made._chat_from_mesh(a_chat_frame(), signed_by=ALICE)
        self.assertEqual(len(made.drawn), 1)
        self.assertEqual(made.chat_misattributed, 0)

    def test_a_line_signed_by_somebody_else_is_refused(self):
        """The attack: Mallory sends a frame carrying Alice's sender_id. Before
        this, the receiver drew Mallory's words as Alice's."""
        made = bridge()
        out = io.StringIO()
        with redirect_stdout(out):
            made._chat_from_mesh(a_chat_frame(), signed_by=MALLORY)
        self.assertEqual(made.drawn, [])
        self.assertEqual(made.chat_misattributed, 1)
        self.assertIn("not rendered", out.getvalue())

    def test_it_is_refused_rather_than_re_attributed(self):
        """Drawing it under the real sender would put words on the screen they
        did not write; drawing it under the claimed one is the attack itself.
        Neither is a rendering worth doing."""
        made = bridge()
        with redirect_stdout(io.StringIO()):
            made._chat_from_mesh(a_chat_frame(), signed_by=MALLORY)
        self.assertEqual(made.drawn, [])

    def test_a_bare_packet_carries_no_proof_and_is_not_pretended_to(self):
        """A GROUP destination gives every member the same key, so nothing
        about a bare packet says who sent it and the sender_id is the only
        claim there is. Honest absence rather than a check that proves
        nothing."""
        made = bridge()
        made._chat_from_mesh(a_chat_frame())
        self.assertEqual(len(made.drawn), 1)
        self.assertEqual(made.chat_misattributed, 0)


class LxmfSourceTests(unittest.TestCase):
    def test_an_lxmf_source_resolves_to_its_member(self):
        """An inbox and a TAK node are different destinations built from one
        identity. That is what lets one be checked against the other."""
        made = bridge()
        identity = object()
        made.rns.Identity.recall.return_value = identity
        with unittest.mock.patch("cot_bridge.tak_identity.node_destination_hash",
                                 return_value=ALICE):
            self.assertEqual(made._member_for_lxmf(b"\x01" * 16), ALICE)

    def test_an_unrecallable_source_proves_nothing(self):
        """None means "no proof available", which leaves the frame's own claim
        standing -- the same position a bare packet is in."""
        made = bridge()
        made.rns.Identity.recall.return_value = None
        self.assertIsNone(made._member_for_lxmf(b"\x01" * 16))

    def test_no_source_hash_proves_nothing(self):
        made = bridge()
        self.assertIsNone(made._member_for_lxmf(None))


if __name__ == "__main__":
    unittest.main()
