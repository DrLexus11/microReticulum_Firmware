"""Asking the propagation node what it is holding.

Escalation only ever put messages into the store. --propagation-node wired the
sending half and left the receiving half to a manual helper, so a bridge that
reconnected after a partition left its own traffic at the command post
indefinitely. Proven on hardware 2026-09-13, where the message arrived only
because an operator pressed sync by hand.
"""

import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_bridge                     # noqa: E402
from cot_bridge import CotBridge      # noqa: E402


def bridge(propagation_node=b"\x11" * 16, with_lxmf=True):
    made = CotBridge.__new__(CotBridge)
    made.identity = object()
    made._last_collect = 0.0
    if with_lxmf:
        made.lxmf = Mock()
        made.lxmf.propagation_node = propagation_node
        made.lxmf.collect.return_value = True
    else:
        made.lxmf = None
    return made


class CollectTests(unittest.TestCase):
    def test_it_asks(self):
        made = bridge()
        self.assertTrue(made.collect_held())
        made.lxmf.collect.assert_called_once_with(made.identity)

    def test_a_second_ask_inside_the_floor_is_refused(self):
        """"A member announced" fires once per member. Ten nodes powering up
        together would otherwise mean ten syncs, and a sync is a Link and a
        transfer rather than one packet."""
        made = bridge()
        made.collect_held()
        self.assertFalse(made.collect_held())
        self.assertEqual(made.lxmf.collect.call_count, 1)

    def test_it_asks_again_once_the_floor_has_passed(self):
        made = bridge()
        made.collect_held()
        made._last_collect -= cot_bridge.COLLECT_FLOOR_SECONDS + 1
        self.assertTrue(made.collect_held())
        self.assertEqual(made.lxmf.collect.call_count, 2)

    def test_with_no_propagation_node_there_is_nobody_to_ask(self):
        made = bridge(propagation_node=None)
        self.assertFalse(made.collect_held())
        made.lxmf.collect.assert_not_called()

    def test_with_lxmf_disabled_it_does_not_ask(self):
        made = bridge(with_lxmf=False)
        self.assertFalse(made.collect_held())


if __name__ == "__main__":
    unittest.main()
