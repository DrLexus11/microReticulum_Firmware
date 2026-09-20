"""Drawing the sender's delivery tick from LXMF's proof, not from the far ATAK.

The tick used to come from the far end: its ATAK wrote a delivered-receipt,
which crossed the mesh as a second LXMF message with its own retry budget, and
the local ATAK drew a tick when it arrived. That is paying twice for one
answer, and the second payment is the one that fails -- a lost receipt leaves
no tick on a message that did arrive.

Measured 2026-09-14: receipts were 11 of 24 outbound LXMF messages, every
message averaged 1.9 packet attempts, and three messages to one destination
were seen retrying against each other at once.
"""

import collections
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_chat                   # noqa: E402
from cot_bridge import CotBridge  # noqa: E402

PEER = b"\x55" * 16


class Registry:
    def describe(self, _hash):
        return {"callsign": "LEXUS"}


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.registry = Registry()
    made.uid = "urtn-" + "aa" * 16
    made.team = "Cyan"
    made.clients = []
    made.clients_lock = threading.Lock()
    made._replay = collections.deque(maxlen=64)
    made._replay_lock = threading.Lock()
    made.replayed = 0
    made.receipts_synthesised = 0
    made.drawn = []
    made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
    return made


def context(message_id="abc-123", room="LEXUS"):
    return {"peer": PEER, "message_id": message_id, "room": room}


class SynthesisTests(unittest.TestCase):
    def test_a_proof_draws_a_delivered_receipt(self):
        made = bridge()
        made._receipt_from_proof(context())
        self.assertEqual(len(made.drawn), 1)
        event = ElementTree.fromstring(made.drawn[0].decode("utf-8"))
        self.assertEqual(event.get("type"), cot_chat.COT_TYPES[cot_chat.KIND_DELIVERED])
        self.assertEqual(made.receipts_synthesised, 1)

    def test_it_is_shaped_exactly_like_the_one_it_replaces(self):
        """Byte-identical in shape to the event that used to cross the mesh, so
        ATAK cannot tell the difference and nothing downstream needs to know."""
        made = bridge()
        made._receipt_from_proof(context(message_id="dead-beef"))
        event = ElementTree.fromstring(made.drawn[0].decode("utf-8"))
        receipt = event.find("detail/__chatreceipt")
        self.assertIsNotNone(receipt)
        self.assertEqual(receipt.get("messageId"), "dead-beef")
        self.assertEqual(event.get("uid"), "dead-beef")

    def test_it_comes_from_the_peer_not_from_us(self):
        """The receipt is the peer's answer, and ATAK keys the conversation on
        the other party. Attributing it to ourselves would file the tick in a
        conversation with ourselves -- the same bug that broke replies."""
        made = bridge()
        made._receipt_from_proof(context())
        event = ElementTree.fromstring(made.drawn[0].decode("utf-8"))
        receipt = event.find("detail/__chatreceipt")
        self.assertEqual(receipt.get("senderCallsign"), "LEXUS")
        self.assertNotEqual(receipt.get("id"), made.uid)

    def test_it_is_held_for_replay_like_any_other_tier_two_event(self):
        made = bridge()
        held = []
        made._to_clients = lambda payload=None, keep=False: held.append(keep)
        made._receipt_from_proof(context())
        self.assertEqual(held, [True])

    def test_a_context_without_a_message_id_draws_nothing(self):
        """Nothing to acknowledge. Drawing a receipt with no id would put a
        tick against whichever message ATAK guessed at."""
        made = bridge()
        made._receipt_from_proof({"peer": PEER, "message_id": None, "room": ""})
        self.assertEqual(made.drawn, [])
        self.assertEqual(made.receipts_synthesised, 0)

    def test_a_context_without_a_peer_draws_nothing(self):
        made = bridge()
        made._receipt_from_proof({"peer": None, "message_id": "abc", "room": ""})
        self.assertEqual(made.drawn, [])


if __name__ == "__main__":
    unittest.main()
