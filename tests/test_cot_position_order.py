"""A position that arrives late must not undo one that arrived on time.

Position is latest-wins, and "latest" was taken to mean "last to arrive". On a
mesh those are different things: a retry, a slower route, or a message held
while a peer was out of range all deliver fixes out of order. Reported from
hardware 2026-09-13 as a track that "reverts, then comes back".
"""

import collections
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import position_codec                 # noqa: E402
from cot_bridge import CotBridge      # noqa: E402

SENDER = b"\x77" * 16


class Registry:
    def resolve_sender_id(self, _sender_id):
        return SENDER

    def describe(self, _hash):
        return {"callsign": "PEER"}


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.registry = Registry()
    made.team = "Cyan"
    made.clients = []
    made.clients_lock = threading.Lock()
    made._replay = collections.deque(maxlen=64)
    made._replay_lock = threading.Lock()
    made.replayed = 0
    made._last_fix = {}
    made.positions_out_of_order = 0
    made.positions_received = 0
    made.unreadable = 0
    made.drawn = []
    made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
    return made


def fix_at(when, lat_e7):
    fix = position_codec.PositionFix(
        lat_e7=lat_e7, lon_e7=290000000, fix_unix_s=when, sender_id=0x11223344)
    return position_codec.encode(fix)


class PositionOrderTests(unittest.TestCase):
    def test_a_newer_fix_is_drawn(self):
        made = bridge()
        made._position_from_mesh(fix_at(1000, 410000000))
        made._position_from_mesh(fix_at(1060, 410001000))
        self.assertEqual(len(made.drawn), 2)
        self.assertEqual(made.positions_out_of_order, 0)

    def test_an_older_fix_arriving_late_is_not_drawn(self):
        """The bug as reported: a track that jumps back to where somebody used
        to be, then returns when the next real fix lands."""
        made = bridge()
        made._position_from_mesh(fix_at(1060, 410001000))
        made._position_from_mesh(fix_at(1000, 410000000))
        self.assertEqual(len(made.drawn), 1)
        self.assertEqual(made.positions_out_of_order, 1)

    def test_the_same_fix_twice_is_not_refused(self):
        """A retry that delivers the same fix again is not out of order, and
        redrawing it changes nothing on the map."""
        made = bridge()
        made._position_from_mesh(fix_at(1000, 410000000))
        made._position_from_mesh(fix_at(1000, 410000000))
        self.assertEqual(len(made.drawn), 2)
        self.assertEqual(made.positions_out_of_order, 0)

    def test_ordering_is_per_sender(self):
        """One peer's clock says nothing about another's. A slow node must not
        be able to silence a fast one."""
        made = bridge()
        first = position_codec.PositionFix(
            lat_e7=410000000, lon_e7=290000000, fix_unix_s=5000,
            sender_id=0xAAAAAAAA)
        second = position_codec.PositionFix(
            lat_e7=410000000, lon_e7=290000000, fix_unix_s=1000,
            sender_id=0xBBBBBBBB)
        made._position_from_mesh(position_codec.encode(first))
        made._position_from_mesh(position_codec.encode(second))
        self.assertEqual(len(made.drawn), 2)
        self.assertEqual(made.positions_out_of_order, 0)


if __name__ == "__main__":
    unittest.main()
