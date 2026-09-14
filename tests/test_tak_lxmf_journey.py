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
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))

try:
    import LXMF
    import tak_lxmf
except ImportError:  # pragma: no cover
    tak_lxmf = None


class Message:
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


if __name__ == "__main__":
    unittest.main()
