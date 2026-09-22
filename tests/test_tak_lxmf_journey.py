"""What a delivery log line has to say to be worth printing.

Counting deliveries cannot tell a message that went out and arrived from one
that failed twice and arrived on the third attempt -- and on a slow channel
that difference is most of the latency an operator feels. Measured 2026-09-13:
a link established in 1.5 s while chat took 11.4 s end to end, and nothing in
the process could say where the other ten seconds went.
"""

import io
import sys
import time
import unittest
import unittest.mock
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

try:
    import LXMF
    import tak_lxmf
except ImportError:  # pragma: no cover
    tak_lxmf = None


class Message:
    # Every real LXMessage has a state, and _failed now reads it: a message
    # LXMF reports as failed may in fact have been cancelled, superseded by a
    # newer version, and must not then be escalated. These are real failures.
    state = LXMF.LXMessage.FAILED if tak_lxmf is not None else None

    def __init__(self, method=None, attempts=None, sent_at=None):
        if method is not None:
            self.method = method
        if attempts is not None:
            self.delivery_attempts = attempts
        if sent_at is not None:
            self.tak_sent_at = sent_at


def carrier():
    made = tak_lxmf.Carrier.__new__(tak_lxmf.Carrier)
    made.delivered = 0
    made.propagation_node = None
    made.failed = 0
    return made


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class JourneyTests(unittest.TestCase):
    def test_it_names_the_carrier_that_actually_moved_it(self):
        """DIRECT means a link carried it. PROPAGATED means it went via the
        command post's store -- correct, but slower, and it means the direct
        attempt failed first. Those are different facts about the mesh and the
        log has to tell them apart."""
        made = carrier()
        line = made._journey(Message(method=LXMF.LXMessage.PROPAGATED))
        self.assertIn("propagated", line)
        line = made._journey(Message(method=LXMF.LXMessage.DIRECT))
        self.assertIn("direct", line)

    def test_it_reports_how_long_rather_than_only_that_it_arrived(self):
        made = carrier()
        line = made._journey(Message(method=LXMF.LXMessage.DIRECT,
                                     sent_at=time.monotonic() - 5))
        self.assertRegex(line, r"in \d\.\ds")

    def test_attempts_are_the_point(self):
        """One attempt and four attempts feel completely different to an
        operator and were previously indistinguishable."""
        made = carrier()
        self.assertIn("lxmf_attempts=4",
                      made._journey(Message(method=LXMF.LXMessage.DIRECT,
                                            attempts=4)))

    def test_a_message_this_carrier_did_not_stamp_still_logs(self):
        """Somebody else's message on a shared router. Unknown is an honest
        answer; raising in a delivery callback would take the router's thread
        down and stop every later message."""
        made = carrier()
        line = made._journey(Message())
        self.assertIn("unknown", line)

    def test_delivery_prints_one_line_and_counts_it(self):
        made = carrier()
        out = io.StringIO()
        with redirect_stdout(out):
            made._delivered(Message(method=LXMF.LXMessage.DIRECT,
                                    attempts=1, sent_at=time.monotonic()))
        self.assertEqual(made.delivered, 1)
        self.assertEqual(out.getvalue().count("\n"), 1)
        self.assertIn("delivery proof received", out.getvalue())

    def test_a_cancelled_message_is_never_escalated(self):
        """LXMF reports a cancellation through the failed callback.

        A drawing's superseded version is cancelled so that it stops costing
        airtime. Escalating it would store it at the propagation node and
        deliver it later -- a shape the operator already moved, arriving after
        the one that replaced it.
        """
        made = carrier()
        made.propagation_node = b"\x01" * 16
        made.router = unittest.mock.Mock()
        cancelled = Message(method=LXMF.LXMessage.OPPORTUNISTIC, attempts=1,
                            sent_at=time.monotonic())
        cancelled.state = LXMF.LXMessage.CANCELLED
        out = io.StringIO()
        with redirect_stdout(out):
            made._failed(cancelled)

        made.router.handle_outbound.assert_not_called()
        self.assertEqual(made.failed, 0)
        self.assertEqual(out.getvalue(), "")

    def test_a_file_that_did_not_land_is_never_escalated(self):
        """A propagation node holding a file would hand it on over whatever
        path the receiver has -- the LoRa leg the fetch gate avoids. The
        receiver asks again on its next fast path instead."""
        made = carrier()
        made.propagation_node = b"\x01" * 16
        made.router = unittest.mock.Mock()
        failed_file = Message(method=LXMF.LXMessage.DIRECT, attempts=5,
                              sent_at=time.monotonic())
        failed_file.tak_escalate = False
        with redirect_stdout(io.StringIO()):
            made._failed(failed_file)

        made.router.handle_outbound.assert_not_called()
        self.assertEqual(made.failed, 1)

    def test_a_failed_direct_says_so_before_escalating(self):
        """The line that would have explained the missing ten seconds."""
        made = carrier()
        out = io.StringIO()
        with redirect_stdout(out):
            made._failed(Message(method=LXMF.LXMessage.DIRECT, attempts=5,
                                 sent_at=time.monotonic()))
        self.assertIn("peer delivery did not land", out.getvalue())
        self.assertIn("lxmf_attempts=5", out.getvalue())

    def test_propagation_ack_does_not_claim_recipient_delivery(self):
        made = carrier()
        out = io.StringIO()
        with redirect_stdout(out):
            made._delivered(Message(method=LXMF.LXMessage.PROPAGATED))
        self.assertIn("stored at propagation node", out.getvalue())
        self.assertNotIn("delivery proof received", out.getvalue())

    def test_identity_and_representation_are_available_for_correlation(self):
        made = carrier()
        message = Message(method=LXMF.LXMessage.DIRECT, attempts=0)
        message.hash = b"x" * 32
        message.representation = LXMF.LXMessage.PACKET
        message.packed_size = 230
        line = made._journey(message)
        self.assertIn("hash=" + message.hash.hex(), line)
        self.assertIn("representation=packet", line)
        self.assertIn("bytes=230", line)


@unittest.skipIf(tak_lxmf is None, "LXMF is not installed in this interpreter")
class ProofTests(unittest.TestCase):
    """When a delivery proof is worth telling the sender about."""

    def make(self, on_proof):
        made = carrier()
        made.on_proof = on_proof
        return made

    def test_a_peer_proof_raises_the_tick(self):
        seen = []
        made = self.make(seen.append)
        message = Message(method=LXMF.LXMessage.OPPORTUNISTIC, attempts=1,
                          sent_at=time.monotonic())
        message.tak_proof_context = {"peer": b"p" * 16, "message_id": "abc"}
        with redirect_stdout(io.StringIO()):
            made._delivered(message)
        self.assertEqual(len(seen), 1)

    def test_stored_at_the_propagation_node_is_not_a_delivery(self):
        """A message sitting in the command post's store has reached nobody.
        Drawing a tick for it would tell an operator their line landed when it
        is still waiting for the recipient to come back."""
        seen = []
        made = self.make(seen.append)
        message = Message(method=LXMF.LXMessage.PROPAGATED, attempts=1,
                          sent_at=time.monotonic())
        message.tak_proof_context = {"peer": b"p" * 16, "message_id": "abc"}
        out = io.StringIO()
        with redirect_stdout(out):
            made._delivered(message)
        self.assertEqual(seen, [])
        self.assertIn("stored at propagation node", out.getvalue())

    def test_a_message_with_no_context_raises_nothing(self):
        """Somebody else's message on a shared router, or one of our own
        receipts, which must not be acknowledged in turn."""
        seen = []
        made = self.make(seen.append)
        with redirect_stdout(io.StringIO()):
            made._delivered(Message(method=LXMF.LXMessage.OPPORTUNISTIC))
        self.assertEqual(seen, [])

    def test_a_raising_callback_does_not_stop_the_router(self):
        """This runs on the router's thread. Raising would stop every later
        message, and a missing tick is worth less than that."""
        def explode(_context):
            raise RuntimeError("no")
        made = self.make(explode)
        message = Message(method=LXMF.LXMessage.OPPORTUNISTIC,
                          sent_at=time.monotonic())
        message.tak_proof_context = {"peer": b"p" * 16, "message_id": "abc"}
        out = io.StringIO()
        with redirect_stdout(out):
            made._delivered(message)      # must not raise
        self.assertIn("could not raise delivery proof", out.getvalue())


if __name__ == "__main__":
    unittest.main()
