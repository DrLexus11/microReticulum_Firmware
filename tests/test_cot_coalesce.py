"""Several versions of one event, and what they would cost without this.

One version of a drawing costs about 7.7 s of channel for a team of seven, and
an operator who edits and re-sends produces several in quick succession. (Not
ATAK's auto-send, which is markers-only and takes the marker codec -- see
tools/cot_coalesce.py.) These pin the two
halves: the sender sends the first version at once and only the latest after
that, and the receiver never draws a version older than one it already drew.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_coalesce                                         # noqa: E402
from cot_coalesce import DROP, HOLD, SEND, event_time       # noqa: E402


class LatestWinsTests(unittest.TestCase):
    def setUp(self):
        self.gate = cot_coalesce.LatestWins(window_seconds=10)

    def test_the_first_version_goes_at_once(self):
        """An operator who draws one shape should not wait for it."""
        self.assertEqual(self.gate.decide("D", "v1", b"1", 0), (SEND, None))

    def test_a_change_inside_the_window_is_held_until_it_closes(self):
        self.gate.decide("D", "v1", b"1", 0)
        self.assertEqual(self.gate.decide("D", "v2", b"2", 3), (HOLD, 10))

    def test_a_drag_costs_one_transfer_per_window_and_the_last_shape_goes(self):
        """Twenty intermediate positions in two seconds, and the channel sees
        the first and the last."""
        self.gate.decide("D", "v0", b"0", 0)
        decisions = [self.gate.decide("D", "v%d" % i, str(i).encode(), i * 0.1)[0]
                     for i in range(1, 21)]

        self.assertEqual(decisions.count(HOLD), 1)
        self.assertEqual(decisions.count(DROP), 19)
        self.assertEqual(self.gate.coalesced, 19)
        self.assertEqual(self.gate.flush("D", 10), "v20")

    def test_after_the_window_the_next_change_goes_at_once(self):
        self.gate.decide("D", "v1", b"1", 0)
        self.assertEqual(self.gate.decide("D", "v2", b"2", 10), (SEND, None))

    def test_an_unchanged_version_is_dropped(self):
        """Whether ATAK re-sent it or a drag came back to where it started, the
        far end already has these bytes."""
        self.gate.decide("D", "v1", b"1", 0)
        self.assertEqual(self.gate.decide("D", "v1", b"1", 30), (DROP, None))
        self.assertEqual(self.gate.identical, 1)

    def test_a_drag_back_to_the_sent_shape_sends_nothing_more(self):
        self.gate.decide("D", "v1", b"1", 0)
        self.gate.decide("D", "v2", b"2", 2)
        self.gate.decide("D", "v1-again", b"1", 4)
        self.assertIsNone(self.gate.flush("D", 10))

    def test_two_drawings_do_not_throttle_each_other(self):
        self.gate.decide("A", "a1", b"a", 0)
        self.assertEqual(self.gate.decide("B", "b1", b"b", 1), (SEND, None))

    def test_a_flush_with_nothing_held_is_nothing(self):
        self.assertIsNone(self.gate.flush("D", 10))

    def test_evicting_a_uid_drops_its_held_version_too(self):
        gate = cot_coalesce.LatestWins(window_seconds=10, max_uids=2)
        gate.decide("A", "a1", b"a1", 0)
        gate.decide("A", "a2", b"a2", 1)          # held
        gate.decide("B", "b1", b"b1", 2)
        gate.decide("C", "c1", b"c1", 3)          # evicts A from _sent
        self.assertNotIn("A", gate._held)

    def test_held_versions_are_bounded_on_their_own(self):
        gate = cot_coalesce.LatestWins(window_seconds=10, max_uids=4)
        for index in range(50):
            uid = "D%d" % index
            gate.decide(uid, "v1", b"1", 0)
            gate.decide(uid, "v2", b"2", 1)       # held for every uid
        self.assertLessEqual(len(gate._held), 4)

    def test_the_memory_does_not_grow_without_bound(self):
        gate = cot_coalesce.LatestWins(window_seconds=10, max_uids=8)
        for index in range(100):
            gate.decide("D%d" % index, "v", str(index).encode(), index)
        self.assertLessEqual(len(gate._sent), 8)


class FreshnessTests(unittest.TestCase):
    """An older version can finish its retries, or come back from the
    propagation node, after a newer one has landed."""

    def setUp(self):
        self.fresh = cot_coalesce.Freshness()

    def test_the_first_version_is_drawn(self):
        self.assertTrue(self.fresh.admit("D", event_time("2026-09-21T07:00:00Z")))

    def test_a_newer_version_is_drawn(self):
        self.fresh.admit("D", event_time("2026-09-21T07:00:00Z"))
        self.assertTrue(self.fresh.admit("D", event_time("2026-09-21T07:00:05Z")))

    def test_an_older_version_arriving_late_is_not_drawn(self):
        """Otherwise a shape the operator already moved goes back on the map."""
        self.fresh.admit("D", event_time("2026-09-21T07:00:05Z"))
        self.assertFalse(self.fresh.admit("D", event_time("2026-09-21T07:00:00Z")))
        self.assertEqual(self.fresh.stale, 1)

    def test_the_same_version_again_is_drawn(self):
        """A replay to a reconnecting client is the same event, not a stale
        one."""
        when = event_time("2026-09-21T07:00:00Z")
        self.fresh.admit("D", when)
        self.assertTrue(self.fresh.admit("D", when))

    def test_times_with_different_precision_still_order(self):
        """ATAK does not always write the same number of fractional digits."""
        self.fresh.admit("D", event_time("2026-09-21T07:00:00.5Z"))
        self.assertFalse(self.fresh.admit("D", event_time("2026-09-21T07:00:00.123Z")))

    def test_an_unplaceable_event_is_admitted(self):
        self.assertTrue(self.fresh.admit("", event_time("2026-09-21T07:00:00Z")))
        self.assertTrue(self.fresh.admit("D", event_time("not a time")))

    def test_uids_are_independent(self):
        self.fresh.admit("A", event_time("2026-09-21T07:00:05Z"))
        self.assertTrue(self.fresh.admit("B", event_time("2026-09-21T07:00:00Z")))


if __name__ == "__main__":
    unittest.main()
