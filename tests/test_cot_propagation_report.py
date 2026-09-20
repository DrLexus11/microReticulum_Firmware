"""Whether a message can survive a partition, said out loud at startup.

Store-and-forward was configurable and completely silent. A wrong hash, or a
node nothing has a route to, looked exactly like a working one right up until
an operator walked out of range and a message vanished -- which is the worst
possible moment to find out, because by then the evidence is gone too.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cot_bridge import CotBridge  # noqa: E402


class FakeTransport:
    def __init__(self, known):
        self.known = known
        self.requested = []

    def has_path(self, destination_hash):
        return self.known

    def request_path(self, destination_hash):
        self.requested.append(destination_hash)


def bridge(propagation_node, path_known=False, with_lxmf=True):
    """A bridge with only the parts the startup report touches."""
    made = CotBridge.__new__(CotBridge)
    made.rns = Mock()
    made.rns.Transport = FakeTransport(path_known)
    if with_lxmf:
        made.lxmf = Mock()
        made.lxmf.propagation_node = propagation_node
    else:
        made.lxmf = None
    return made


def report(made):
    out = io.StringIO()
    with redirect_stdout(out):
        made._report_propagation()
    return out.getvalue()


class PropagationReportTests(unittest.TestCase):
    def test_a_configured_node_is_named(self):
        made = bridge(b"\x11" * 16, path_known=True)
        self.assertIn("1111111111111111", report(made))

    def test_no_propagation_node_says_what_that_costs(self):
        """The state that loses messages, so it does not get to be the quiet
        one. An operator reading this should know before the exercise that a
        peer out of range means a failure, not a wait."""
        made = bridge(None)
        line = report(made)
        self.assertIn("no propagation node", line)
        self.assertIn("out of range", line)

    def test_a_node_with_no_path_yet_says_so_and_asks_for_one(self):
        """Asked for rather than only reported. At startup there usually is no
        path yet, and resolving it during a partition means resolving it over
        the link that is already in trouble."""
        node = b"\x22" * 16
        made = bridge(node, path_known=False)
        self.assertIn("not yet known", report(made))
        self.assertEqual(made.rns.Transport.requested, [node])

    def test_a_known_path_is_not_re_requested(self):
        made = bridge(b"\x33" * 16, path_known=True)
        self.assertIn("path known", report(made))
        self.assertEqual(made.rns.Transport.requested, [])

    def test_lxmf_disabled_reports_nothing(self):
        """--no-lxmf is a deliberate choice, not a degraded state, and a bridge
        running without it should not be told about a facility it opted out
        of."""
        made = bridge(None, with_lxmf=False)
        self.assertEqual(report(made), "")


if __name__ == "__main__":
    unittest.main()
