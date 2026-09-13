"""Which carrier an addressed chat line actually leaves on.

PR C's claim is that a direct message is delivered, retried, and held for
somebody out of range. That claim is only true if the line goes by LXMF, so the
one thing worth asserting here is that a failure to use LXMF is never quietly
turned into something weaker that looks the same from outside.
"""

import io
import sys
import threading
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge  # noqa: E402

PEER = b"\x42" * 16


class Registry:
    def members(self):
        return [PEER]

    def describe(self, _hash):
        return {"callsign": "PEER"}


def bridge(lxmf, recall=True):
    """A bridge with only the parts the addressed-chat decision touches."""
    made = CotBridge.__new__(CotBridge)
    made.rns = Mock()
    made.rns.Identity.recall.return_value = object() if recall else None
    made.lxmf = lxmf
    made.registry = Registry()
    made.clients = []
    made.clients_lock = threading.Lock()
    made.chat_sent = 0
    made.chat_undeliverable = 0
    made.unreachable = 0
    made._send_to = Mock(return_value=1)
    return made


class Lxmf:
    def __init__(self, accepts):
        self.accepts = accepts
        self.calls = 0

    def send_chat(self, identity, frame, text=""):
        self.calls += 1
        return self.accepts


class AddressedChatTests(unittest.TestCase):
    def test_a_line_lxmf_accepts_does_not_also_go_as_a_packet(self):
        made = bridge(Lxmf(accepts=True))
        with redirect_stdout(io.StringIO()):
            made._dispatch_addressed_chat(PEER, b"frame", {"text": "hello"})
        self.assertEqual(made.chat_sent, 1)
        made._send_to.assert_not_called()

    def test_an_lxmf_failure_is_not_downgraded_to_a_bare_packet(self):
        """The behaviour this replaces looked like graceful degradation and was
        not. A bare packet has no proof, no retry and no propagation node, so
        the sender was told the line went while the one guarantee they were
        relying on had quietly been dropped -- and the two outcomes are
        indistinguishable from outside the process.

        It also fires on a *construction* error, where a second attempt at the
        same frame is no likelier to work. --no-lxmf is how somebody chooses
        best effort; it is not something to be handed to them by accident."""
        made = bridge(Lxmf(accepts=False))
        out = io.StringIO()
        with redirect_stdout(out):
            made._dispatch_addressed_chat(PEER, b"frame", {"text": "hello"})
        made._send_to.assert_not_called()
        self.assertEqual(made.chat_sent, 0)
        self.assertEqual(made.chat_undeliverable, 1)
        self.assertIn("not falling back", out.getvalue())

    def test_a_peer_whose_identity_we_cannot_recall_still_takes_the_packet(self):
        """The fallback that was always intended: not an LXMF failure, but a
        peer we cannot build a link to at all. LXMF is never even asked."""
        lxmf = Lxmf(accepts=True)
        made = bridge(lxmf, recall=False)
        with redirect_stdout(io.StringIO()):
            made._dispatch_addressed_chat(PEER, b"frame", {"text": "hello"})
        self.assertEqual(lxmf.calls, 0)
        made._send_to.assert_called_once()
        self.assertEqual(made.chat_sent, 1)

    def test_with_lxmf_disabled_the_packet_is_the_route(self):
        """--no-lxmf is a deliberate choice of best effort, and it still
        delivers what it can."""
        made = bridge(None)
        with redirect_stdout(io.StringIO()):
            made._dispatch_addressed_chat(PEER, b"frame", {"text": "hello"})
        made._send_to.assert_called_once()
        self.assertEqual(made.chat_sent, 1)


if __name__ == "__main__":
    unittest.main()
