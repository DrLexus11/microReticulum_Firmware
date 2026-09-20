"""Greeting: who gets answered when they announce, and how often.

CarriedIssues #5. A node that *restarts* loses its own membership while
everybody else still remembers it, so greeting only members that are new to us
left it greeted by nobody -- on the air, visible to its peers, and unable to
resolve a single sender id until the next scheduled announce.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_membership as membership
from cot_bridge import TeamAnnounceHandler

SECRET = b"fleet secret for the greeting tests"
TEAM = "Cyan"
PEER = b"\x11" * 16
OTHER = b"\x22" * 16


class GreetingTests(unittest.TestCase):
    def setUp(self):
        self.registry = membership.MemberRegistry(TEAM, SECRET)
        self.greeted = []
        self.joined = []
        self.handler = TeamAnnounceHandler(
            self.registry,
            self.joined.append,
            lambda dest, outcome: self.greeted.append(outcome),
        )
        self.payload = membership.member_payload(TEAM, SECRET, "PEER")

    def announce(self, destination=PEER, payload=None):
        self.handler.received_announce(destination, None,
                                       self.payload if payload is None else payload)

    def test_a_member_we_have_never_heard_is_greeted(self):
        self.announce()
        self.assertEqual(self.greeted, [membership.MEMBER_NEW])

    def test_a_member_we_already_know_is_greeted_too(self):
        """The fix. A peer that restarted is not new to us, and it is exactly
        the peer that needs telling who we are -- it has forgotten."""
        self.announce()
        self.registry._last_greet = None      # the rate limit is tested below
        self.announce()
        self.assertEqual(self.greeted,
                         [membership.MEMBER_NEW, membership.MEMBER_KNOWN])

    def test_the_rate_limit_is_what_bounds_it(self):
        """Ten nodes powering up together greet once each inside the floor
        rather than nine times, which is the most expensive packet Reticulum
        has.

        Modelled the way the bridge does it: the handler fires on every
        announce, and the registry decides whether that becomes an announce on
        the air. Separating the two is the point -- the trigger is broad so a
        restarted node is answered, and the floor is what keeps it cheap.
        """
        sent = []
        handler = TeamAnnounceHandler(
            self.registry, None,
            lambda dest, outcome: self.registry.should_greet() and sent.append(dest),
        )
        for _ in range(5):
            handler.received_announce(PEER, None, self.payload)
        self.assertEqual(len(sent), 1,
                         "five announces inside the floor must cost one greeting")

    def test_the_floor_opens_again(self):
        self.assertTrue(self.registry.should_greet())
        self.assertFalse(self.registry.should_greet())
        self.registry._last_greet -= membership.GREET_MIN_INTERVAL_SECONDS + 1
        self.assertTrue(self.registry.should_greet())

    def test_somebody_elses_team_is_neither_greeted_nor_joined(self):
        """An announce on our aspect that does not carry our tag. Greeting it
        would tell a node on another fleet secret that we are here."""
        stranger = membership.member_payload(TEAM, b"a different fleet secret", "X")
        self.announce(OTHER, stranger)
        self.assertEqual(self.greeted, [])
        self.assertEqual(self.joined, [])

    def test_only_a_member_that_was_new_is_announced_as_joining(self):
        """The log line is still about arrival, not about every announce, or a
        team of ten would print a line a minute for nodes nobody has lost."""
        self.announce()
        self.registry._last_greet = None
        self.announce()
        self.assertEqual(self.joined, [PEER])


if __name__ == "__main__":
    unittest.main()
