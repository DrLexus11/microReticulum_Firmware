"""Membership, which is what a team is once a GROUP address cannot carry one."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_groups as groups
import tak_membership as membership

SECRET = b"a fleet secret long enough to pass"
OTHER = b"a different fleet secret entirely!"


class TeamTagTests(unittest.TestCase):
    def test_the_team_name_never_reaches_the_air(self):
        """group_aspects goes to some trouble to keep team names off the air.
        An announce saying team="Cyan" would hand a listener exactly that."""
        payload = membership.member_payload("Cyan", SECRET, "LEXUS")
        for spelling in (b"Cyan", b"cyan", b"CYAN"):
            self.assertNotIn(spelling, payload)

    def test_the_tag_is_stable_for_everyone_holding_the_secret(self):
        self.assertEqual(membership.team_tag("Cyan", SECRET),
                         membership.team_tag(" CYAN ", SECRET))

    def test_a_different_team_or_secret_gives_a_different_tag(self):
        self.assertNotEqual(membership.team_tag("Cyan", SECRET),
                            membership.team_tag("Magenta", SECRET))
        self.assertNotEqual(membership.team_tag("Cyan", SECRET),
                            membership.team_tag("Cyan", OTHER))

    def test_a_secret_too_weak_for_a_group_key_cannot_claim_membership(self):
        with self.assertRaises(ValueError):
            membership.team_tag("Cyan", b"too short")

    def test_the_tag_is_domain_separated_from_the_other_derivations(self):
        """One secret feeds the group key, the identity, the aspect label and
        now this. They must not be the same value under different names."""
        tag = membership.team_tag("Cyan", SECRET)
        self.assertNotIn(tag, groups.group_key("Cyan", SECRET))
        self.assertNotIn(tag, groups.group_identity_key("Cyan", SECRET))
        self.assertNotIn(tag.hex(), groups.group_aspects("Cyan", SECRET)[-1])


class PayloadTests(unittest.TestCase):
    def test_a_payload_round_trips(self):
        payload = membership.member_payload("Cyan", SECRET, "LEXUS", "Team Lead")
        claims = membership.parse_member(payload)
        self.assertEqual(claims["callsign"], "LEXUS")
        self.assertEqual(claims["role"], "Team Lead")
        self.assertEqual(claims["tag"], membership.team_tag("Cyan", SECRET))

    def test_a_payload_fits_an_announce(self):
        """app_data rides in the announce packet, which is small. A callsign at
        the limit must not be the thing that makes an announce undeliverable."""
        payload = membership.member_payload("Cyan", SECRET, "L" * membership.MAX_CALLSIGN,
                                            "R" * membership.MAX_ROLE)
        self.assertLessEqual(len(payload), 128)

    def test_a_non_ascii_callsign_survives(self):
        claims = membership.parse_member(
            membership.member_payload("Cyan", SECRET, "Göktürk"))
        self.assertEqual(claims["callsign"], "Göktürk")

    def test_an_announce_that_is_not_ours_is_not_an_error(self):
        """Announces arrive from every node on the mesh, including ones running
        other software. Most of what this sees is not ours."""
        for foreign in (None, b"", b"\x01\x02", bytes(11), b"x" * 200,
                        bytes([1]) + bytes(10)):
            self.assertIsNone(membership.parse_member(foreign), repr(foreign))

    def test_a_truncated_payload_is_refused(self):
        payload = membership.member_payload("Cyan", SECRET, "LEXUS")
        for cut in range(1, len(payload)):
            self.assertIsNone(membership.parse_member(payload[:cut]), cut)

    def test_trailing_bytes_are_refused(self):
        """The lengths in the header describe the whole body. Anything after it
        is not something this version knows how to read."""
        payload = membership.member_payload("Cyan", SECRET, "LEXUS")
        self.assertIsNone(membership.parse_member(payload + b"extra"))

    def test_invalid_utf8_is_refused_not_substituted(self):
        payload = bytearray(membership.member_payload("Cyan", SECRET, "LEXUS"))
        payload[membership.HEADER_BYTES] = 0xFF
        self.assertIsNone(membership.parse_member(bytes(payload)))

    def test_control_characters_are_refused(self):
        with self.assertRaises(ValueError):
            membership.member_payload("Cyan", SECRET, "LEX\x00US")


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.own = bytes([0xAA] * 16)
        self.registry = membership.MemberRegistry("Cyan", SECRET, own_hash=self.own)
        self.peer = bytes([0x01] * 16)

    def ours(self, callsign="PEER"):
        return membership.member_payload("Cyan", SECRET, callsign)

    def test_an_announce_from_our_team_makes_a_member(self):
        self.assertEqual(self.registry.remember(self.peer, self.ours(), now=100),
                         membership.MEMBER_NEW)
        self.assertEqual(self.registry.members(now=100), [self.peer])

    def test_another_team_is_not_a_member_and_is_not_paid_for(self):
        """Addressed traffic costs a transmission per member, so a stranger in
        the list is airtime spent on somebody else's exercise."""
        payload = membership.member_payload("Magenta", SECRET, "THEIRS")
        self.assertIsNone(self.registry.remember(self.peer, payload, now=100))
        self.assertEqual(self.registry.members(now=100), [])

    def test_a_forged_tag_needs_the_fleet_secret(self):
        payload = membership.member_payload("Cyan", OTHER, "IMPOSTOR")
        self.assertIsNone(self.registry.remember(self.peer, payload, now=100))

    def test_our_own_announce_is_not_a_member(self):
        """It comes back through Transport like anyone else's. Addressing
        ourselves would double every marker and feed our own events back into
        our own endpoint."""
        self.assertIsNone(self.registry.remember(self.own, self.ours("SELF"), now=100))
        self.assertEqual(self.registry.members(now=100), [])

    def test_a_later_announce_updates_rather_than_duplicates(self):
        self.registry.remember(self.peer, self.ours("OLD"), now=100)
        self.registry.remember(self.peer, self.ours("NEW"), now=200)
        self.assertEqual(len(self.registry), 1)
        self.assertEqual(self.registry.describe(self.peer)["callsign"], "NEW")

    def test_a_member_is_kept_far_longer_than_an_announce_interval(self):
        """Forgetting a member means silently declining to send to them, and a
        responder who has gone quiet is exactly who is still worth addressing."""
        self.registry.remember(self.peer, self.ours(), now=0)
        self.assertEqual(self.registry.members(now=membership.DEFAULT_EXPIRY_SECONDS - 1),
                         [self.peer])
        self.assertEqual(self.registry.members(now=membership.DEFAULT_EXPIRY_SECONDS + 1), [])

    def test_expired_members_can_be_dropped_explicitly(self):
        self.registry.remember(self.peer, self.ours(), now=0)
        self.assertEqual(self.registry.forget_expired(now=0), 0)
        self.assertEqual(self.registry.forget_expired(now=membership.DEFAULT_EXPIRY_SECONDS + 1), 1)
        self.assertEqual(len(self.registry), 0)

    def test_members_are_ordered_by_how_recently_they_were_heard(self):
        second = bytes([0x02] * 16)
        self.registry.remember(self.peer, self.ours("FIRST"), now=100)
        self.registry.remember(second, self.ours("SECOND"), now=200)
        self.assertEqual(self.registry.members(now=300), [second, self.peer])

    def test_describe_is_a_copy_so_a_caller_cannot_edit_the_registry(self):
        self.registry.remember(self.peer, self.ours("PEER"), now=100)
        self.registry.describe(self.peer)["callsign"] = "TAMPERED"
        self.assertEqual(self.registry.describe(self.peer)["callsign"], "PEER")


class GreetTests(unittest.TestCase):
    """A node that starts late hears everyone who announces after it and
    nobody who announced before. Without greeting, the first node up stays
    invisible to the second until the next re-announce -- observed on the
    bench as ALPHA seeing BRAVO while BRAVO saw nobody."""

    def setUp(self):
        self.registry = membership.MemberRegistry("Cyan", SECRET)

    def test_the_first_meeting_prompts_an_announce(self):
        self.assertTrue(self.registry.should_greet(now=100))

    def test_a_burst_of_new_members_does_not_become_a_burst_of_announces(self):
        """Ten nodes powering up together would otherwise each announce nine
        times, which is a lot of the most expensive packet Reticulum has."""
        self.assertTrue(self.registry.should_greet(now=100))
        for offset in range(1, membership.GREET_MIN_INTERVAL_SECONDS):
            self.assertFalse(self.registry.should_greet(now=100 + offset), offset)

    def test_greeting_is_allowed_again_after_the_floor(self):
        self.assertTrue(self.registry.should_greet(now=100))
        self.assertTrue(
            self.registry.should_greet(now=100 + membership.GREET_MIN_INTERVAL_SECONDS))

    def test_greeting_converges_rather_than_ping_pongs(self):
        """A greeting is sent only for a member that was not already known, so
        the reply it provokes finds a known member and stops there."""
        peer = bytes([0x01] * 16)
        payload = membership.member_payload("Cyan", SECRET, "PEER")
        self.assertEqual(self.registry.remember(peer, payload, now=100),
                         membership.MEMBER_NEW)
        self.assertEqual(self.registry.remember(peer, payload, now=101),
                         membership.MEMBER_KNOWN,
                         "a second announce from a known member is not a new arrival")

    def test_a_member_heard_again_after_expiry_is_a_new_arrival(self):
        """From their side we may equally have dropped off, so this is worth
        greeting rather than treating as a refresh."""
        registry = membership.MemberRegistry("Cyan", SECRET)
        peer = bytes([0x03] * 16)
        payload = membership.member_payload("Cyan", SECRET, "PEER")
        self.assertEqual(registry.remember(peer, payload, now=0), membership.MEMBER_NEW)
        self.assertEqual(
            registry.remember(peer, payload, now=membership.DEFAULT_EXPIRY_SECONDS + 1),
            membership.MEMBER_NEW)


if __name__ == "__main__":
    unittest.main()
