"""A restarted ATAK gets every peer's last position back, and a member that
announces gets ours: the stationary-ATAK fix (TAKDeliveryPlan, open before
Outdoor Test 1). The Python halves of Columba's LastPositions and OwnPosition."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_position  # noqa: E402
from cot_bridge import LastPositions, OwnPosition  # noqa: E402


class LastPositionsTests(unittest.TestCase):
    def test_each_peer_is_replayed_once_at_its_latest(self):
        kept = LastPositions()
        kept.remember("a", b"<a1/>", stale_at=1000)
        kept.remember("b", b"<b1/>", stale_at=1000)
        kept.remember("a", b"<a2/>", stale_at=2000)
        self.assertEqual([b"<b1/>", b"<a2/>"], kept.for_replay(now=500))

    def test_a_stale_position_is_replayed_as_drawn_until_the_hold_runs_out(self):
        kept = LastPositions(hold_s=10)
        kept.remember("a", b"<a/>", stale_at=1)
        self.assertEqual([b"<a/>"], kept.for_replay(now=5))
        self.assertEqual([], kept.for_replay(now=11.001))

    def test_the_oldest_peer_gives_way_when_full(self):
        kept = LastPositions(max_peers=2)
        for uid in "abc":
            kept.remember(uid, uid.encode(), stale_at=1)
        self.assertEqual([b"b", b"c"], kept.for_replay(now=0))


class OwnPositionTests(unittest.TestCase):
    def test_nothing_sent_nothing_offered(self):
        self.assertIsNone(OwnPosition().current(0))

    def test_a_report_holds_for_twice_its_stated_cadence(self):
        own = OwnPosition()
        own.record(b"x", interval_min=3, fix_s=0, now=0)
        self.assertEqual(b"x", own.current(360))
        self.assertIsNone(own.current(360.001))

    def test_an_unstated_cadence_holds_for_twice_the_default(self):
        own = OwnPosition()
        own.record(b"x", interval_min=0, fix_s=0, now=0)
        self.assertEqual(b"x", own.current(2 * cot_position.DEFAULT_INTERVAL_SECONDS))
        self.assertIsNone(own.current(2 * cot_position.DEFAULT_INTERVAL_SECONDS + 0.001))

    def test_an_older_fix_finishing_later_does_not_replace_a_newer_one(self):
        own = OwnPosition()
        own.record(b"new", interval_min=1, fix_s=2, now=2)
        own.record(b"old", interval_min=1, fix_s=1, now=2.1)
        self.assertEqual(b"new", own.current(2.2))

    def test_each_member_is_answered_once_per_window_counted_only_once_sent(self):
        own = OwnPosition(answer_every_s=600)
        own.record(b"x", interval_min=10, fix_s=0, now=0)
        self.assertEqual(b"x", own.offer_for("a", 0))
        # The send failed: nothing was counted, so the next announce is answered.
        self.assertEqual(b"x", own.offer_for("a", 1))
        own.answered("a", 1)
        self.assertIsNone(own.offer_for("a", 600))
        self.assertEqual(b"x", own.offer_for("b", 2))
        self.assertEqual(b"x", own.offer_for("a", 601))

if __name__ == "__main__":
    unittest.main()
