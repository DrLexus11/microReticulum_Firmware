"""Noticing that this node has been talking to nobody.

A transport can go deaf without failing. A Rev 2 was found doing exactly that
on 2026-09-13: answering ping for three hours while its Reticulum side had
stopped transmitting. Nothing on the deck noticed -- the bridge kept
announcing, kept reporting itself healthy, and the first sign of trouble was an
operator noticing their phone was missing from the map.
"""

import io
import sys
import time
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_bridge                     # noqa: E402
from cot_bridge import CotBridge      # noqa: E402


class Registry:
    def __init__(self, members):
        self._members = members

    def members(self):
        return self._members


def bridge(members=(b"\x11" * 16,)):
    made = CotBridge.__new__(CotBridge)
    made.registry = Registry(list(members))
    made._last_mesh_input = time.time()
    made._silence_reported = False
    return made


def check(made):
    out = io.StringIO()
    with redirect_stdout(out):
        made._check_mesh_silence()
    return out.getvalue()


class SilenceTests(unittest.TestCase):
    def test_a_recently_active_mesh_says_nothing(self):
        self.assertEqual(check(bridge()), "")

    def test_silence_with_members_known_is_reported(self):
        made = bridge()
        made._last_mesh_input -= cot_bridge.MESH_SILENCE_SECONDS + 1
        line = check(made)
        self.assertIn("nothing has arrived from the mesh", line)
        # Pointed at the radio, because announces are still leaving: the team
        # being quiet and the transport being deaf look identical from here
        # otherwise, and they want different actions.
        self.assertIn("check the radio", line)

    def test_a_node_that_knows_nobody_is_alone_not_deaf(self):
        """Those want different answers from an operator, so they must not
        produce the same warning."""
        made = bridge(members=())
        made._last_mesh_input -= cot_bridge.MESH_SILENCE_SECONDS + 1
        self.assertEqual(check(made), "")

    def test_it_is_said_once_per_episode(self):
        """A warning that repeats every announce interval is a warning people
        learn to scroll past."""
        made = bridge()
        made._last_mesh_input -= cot_bridge.MESH_SILENCE_SECONDS + 1
        self.assertNotEqual(check(made), "")
        self.assertEqual(check(made), "")

    def test_recovery_is_announced_too(self):
        """An operator who saw the warning needs to know it is over, rather
        than inferring it from traffic they cannot see."""
        made = bridge()
        made._last_mesh_input -= cot_bridge.MESH_SILENCE_SECONDS + 1
        check(made)
        out = io.StringIO()
        with redirect_stdout(out):
            made._heard_mesh()
        self.assertIn("resumed", out.getvalue())
        # And having recovered, it can warn again if it happens twice.
        made._last_mesh_input -= cot_bridge.MESH_SILENCE_SECONDS + 1
        self.assertNotEqual(check(made), "")

    def test_ordinary_traffic_is_quiet(self):
        made = bridge()
        out = io.StringIO()
        with redirect_stdout(out):
            made._heard_mesh()
        self.assertEqual(out.getvalue(), "")


if __name__ == "__main__":
    unittest.main()
