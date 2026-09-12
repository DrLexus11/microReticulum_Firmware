"""GeoChat and receipts, the last of the 853 events without a typed codec."""

import json
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_chat
import cot_tier2
import position_codec
import tak_payload

FIXTURES = json.loads(
    (Path(__file__).resolve().parents[1] / "tests/fixtures/tak_native_v1.json").read_text())
CHAT = FIXTURES["chat"]
SENDER = 0x11223344


class PayloadKindTests(unittest.TestCase):
    """Byte zero is one namespace shared by every codec, not three independent
    version counters. A tier 2 frame read as a position does not fail cleanly
    -- it decodes into coordinates, and a marker lands where nobody put it."""

    def test_every_codec_takes_a_distinct_kind(self):
        kinds = [cot_tier2.VERSION, position_codec.WIRE_VERSION, cot_chat.VERSION]
        self.assertEqual(len(kinds), len(set(kinds)))

    def test_every_kind_in_use_is_registered(self):
        for version in (cot_tier2.VERSION, position_codec.WIRE_VERSION, cot_chat.VERSION):
            self.assertIn(version, tak_payload.KINDS)

    def test_an_unregistered_kind_is_not_guessed_at(self):
        self.assertIsNone(tak_payload.kind_of(bytes([99, 0, 0])))
        self.assertIsNone(tak_payload.kind_of(b""))
        self.assertEqual(tak_payload.name_of(bytes([99])), "unknown")

    def test_each_codec_refuses_the_others_frames(self):
        chat = cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER,
                               "3e80bd07-fdd8-4c2d-aaf4-8119b1f23a56", "Cyan", "hello")
        self.assertIsNone(position_codec.decode(chat))
        with self.assertRaises(ValueError):
            cot_tier2.decode(chat)
        self.assertIsNone(cot_chat.decode(cot_tier2.encode("<event uid='a'/>")))


class RealEventTests(unittest.TestCase):
    """Against events ATAK actually produced in this lab, taken from the
    OpenTAKServer database and stripped of the identifiers that came with
    them."""

    def test_a_real_message_does_not_fit_tier_2_at_all(self):
        """The justification for this codec in one assertion: a GeoChat line
        compresses to more than one Reticulum packet, so before this existed
        chat was not merely expensive, it was undeliverable."""
        with self.assertRaises(ValueError) as caught:
            cot_tier2.encode(CHAT["message"])
        self.assertIn("tier 3", str(caught.exception))

    def test_a_real_message_becomes_tens_of_bytes(self):
        """This one addresses a WinTAK user, whose uid is a 44-character
        Windows SID, so most of the frame is the recipient. Carrying it is what
        stops a private line reaching the whole team, and it was measured
        against compacting a urtn- recipient to sixteen raw bytes: twenty-one
        bytes on an event an operator types by hand did not justify a second
        encoding and a second way to get it wrong."""
        frame = cot_chat.chat_from_cot(CHAT["message"], SENDER)
        self.assertIsNotNone(frame)
        self.assertLess(len(frame), 100)
        self.assertLess(len(frame), len(CHAT["message"]) // 10)

    def test_a_real_message_keeps_what_matters(self):
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        self.assertEqual(decoded["kind"], cot_chat.KIND_MESSAGE)
        self.assertEqual(decoded["room"], "Inquisitor")
        self.assertEqual(decoded["text"], "where u at")
        self.assertEqual(decoded["sender_id"], SENDER)

    def test_a_real_receipt_carries_no_words(self):
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["receipt_delivered"], SENDER))
        self.assertEqual(decoded["kind"], cot_chat.KIND_DELIVERED)
        self.assertEqual(decoded["text"], "")
        self.assertEqual(decoded["room"], "Inquisitor")

    def test_the_frames_match_the_stored_vectors(self):
        for name, key in (("message", "message_frame"),
                          ("receipt_delivered", "receipt_frame")):
            self.assertEqual(cot_chat.chat_from_cot(CHAT[name], SENDER).hex(), CHAT[key], name)

    def test_a_rebuilt_event_survives_another_round_trip(self):
        """What the far end hands ATAK has to be something this codec would
        recognise again, or a message relayed twice would decay."""
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        rebuilt = cot_chat.build_chat_cot(decoded, "urtn-" + "ab" * 16, "LEXUS",
                                          "2026-09-11T20:00:00.000Z")
        again = cot_chat.decode(cot_chat.chat_from_cot(rebuilt, SENDER))
        self.assertEqual(again, decoded)

    def test_a_position_report_is_not_chat(self):
        self.assertIsNone(cot_chat.chat_from_cot(FIXTURES["tier2"]["cot"], SENDER))


class CodecTests(unittest.TestCase):
    MESSAGE_ID = "3e80bd07-fdd8-4c2d-aaf4-8119b1f23a56"

    def test_a_message_round_trips(self):
        frame = cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan", "on my way")
        decoded = cot_chat.decode(frame)
        self.assertEqual(decoded["message_id"], self.MESSAGE_ID)
        self.assertEqual(decoded["text"], "on my way")

    def test_a_message_id_is_sixteen_bytes_not_thirty_six(self):
        """A receipt is mostly this field, so the text form would more than
        double one."""
        frame = cot_chat.encode(cot_chat.KIND_DELIVERED, SENDER, self.MESSAGE_ID, "Cyan")
        self.assertNotIn(self.MESSAGE_ID.encode(), frame)
        self.assertEqual(cot_chat.decode(frame)["message_id"], self.MESSAGE_ID)

    def test_a_non_ascii_line_survives(self):
        decoded = cot_chat.decode(
            cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan", "yoldayız"))
        self.assertEqual(decoded["text"], "yoldayız")

    def test_a_receipt_carrying_words_is_refused(self):
        with self.assertRaises(ValueError):
            cot_chat.encode(cot_chat.KIND_READ, SENDER, self.MESSAGE_ID, "Cyan", "sneaky")

    def test_a_message_with_no_words_is_refused(self):
        with self.assertRaises(ValueError):
            cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan", "")

    def test_a_message_id_that_is_not_a_uuid_is_refused(self):
        with self.assertRaises(ValueError):
            cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, "not-a-uuid", "Cyan", "hi")

    def test_an_over_long_line_is_left_to_tier_2(self):
        """Refused rather than truncated: silently cutting somebody's words is
        worse than spending the airtime to carry them."""
        with self.assertRaises(ValueError):
            cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan",
                            "x" * (cot_chat.MAX_TEXT + 1))

    def test_truncated_and_padded_frames_are_refused(self):
        frame = cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan", "hi")
        for cut in range(1, len(frame)):
            self.assertIsNone(cot_chat.decode(frame[:cut]), cut)
        self.assertIsNone(cot_chat.decode(frame + b"extra"))

    def test_a_kind_that_disagrees_with_its_own_body_is_refused(self):
        """A receipt with text, or a message without, is not what it says it
        is -- and a caller trusting `kind` would act on the wrong thing."""
        frame = bytearray(cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER,
                                          self.MESSAGE_ID, "Cyan", "hi"))
        frame[1] = cot_chat.KIND_READ
        self.assertIsNone(cot_chat.decode(bytes(frame)))

    def test_invalid_utf8_is_refused_not_substituted(self):
        frame = bytearray(cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER,
                                          self.MESSAGE_ID, "Cyan", "hi"))
        frame[-1] = 0xFF
        self.assertIsNone(cot_chat.decode(bytes(frame)))

    def test_markup_in_a_line_cannot_escape_the_rebuilt_event(self):
        """A peer chooses its own words and this renders them into XML."""
        decoded = cot_chat.decode(
            cot_chat.encode(cot_chat.KIND_MESSAGE, SENDER, self.MESSAGE_ID, "Cyan",
                            '</remarks><detail evil="1">'))
        rebuilt = cot_chat.build_chat_cot(decoded, "urtn-x", "PEER",
                                          "2026-09-11T20:00:00.000Z")
        again = cot_chat.decode(cot_chat.chat_from_cot(rebuilt, SENDER))
        self.assertEqual(again["text"], '</remarks><detail evil="1">')


class AddressingTests(unittest.TestCase):
    """ATAK puts the recipient's *callsign* in the chatroom field for a direct
    message, so the room alone cannot tell "everyone" from "one person". Until
    the recipient was carried separately, every private line was delivered to
    the whole team -- not a cost problem, a confidentiality one."""

    def test_a_direct_message_carries_who_it_is_for(self):
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        self.assertTrue(decoded["recipient"])
        self.assertNotEqual(decoded["recipient"], decoded["room"])

    def addressed_to(self, target):
        """The captured message, readdressed. Derived from the fixture rather
        than from a literal: the identifiers in it have been re-sanitised once
        already, and a hard-coded copy silently stopped matching."""
        recipient = CHAT["message_recipient"]
        return CHAT["message"].replace(recipient, target)

    def test_a_broadcast_carries_no_recipient(self):
        """A line to everyone has no single addressee, and inventing one would
        narrow a broadcast to one person -- the same bug in reverse."""
        for room in ("All Chat Rooms", "All Streaming"):
            event = self.addressed_to(room).replace('chatroom="Inquisitor"',
                                                    'chatroom="%s"' % room)
            decoded = cot_chat.decode(cot_chat.chat_from_cot(event, SENDER))
            self.assertEqual(decoded["recipient"], "", room)

    def test_a_room_with_three_people_is_not_a_direct_message(self):
        """chatgrp enumerates participants. Reading uid1 as a recipient in a
        room would narrow the conversation to whoever is listed second, which
        is the confidentiality bug pointing at the wrong person instead of at
        everybody."""
        event = CHAT["message"].replace(
            'uid1="%s"' % CHAT["message_recipient"],
            'uid1="%s" uid2="ANDROID-1111111111111111"' % CHAT["message_recipient"])
        decoded = cot_chat.decode(cot_chat.chat_from_cot(event, SENDER))
        self.assertEqual(decoded["recipient"], "")

    def test_a_team_room_is_not_a_direct_message(self):
        """The room's own name in the id field is a room, not a person. ATAK
        writes it that way for a team chat."""
        event = (CHAT["message"]
                 .replace('id="%s"' % CHAT["message_recipient"], 'id="Inquisitor"')
                 .replace('uid1="%s"' % CHAT["message_recipient"], 'uid1="Inquisitor"'))
        decoded = cot_chat.decode(cot_chat.chat_from_cot(event, SENDER))
        self.assertEqual(decoded["recipient"], "")

    def test_a_uid_addressed_line_resolves_to_one_peer(self):
        """Peers are announced under their Reticulum-rooted UID, so ATAK
        addresses them by it and destination_for() reverses it. Pivot 1 paying
        for itself."""
        import tak_identity
        peer = "urtn-" + "cd" * 16
        decoded = cot_chat.decode(cot_chat.chat_from_cot(self.addressed_to(peer), SENDER))
        self.assertEqual(decoded["recipient"], peer)
        self.assertIsNotNone(tak_identity.destination_for(decoded["recipient"]))

    def test_threading_uses_a_uid_not_a_callsign(self):
        """chatgrp uid1 named the room, which is a callsign for a direct
        message. ATAK threads on the uid."""
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        rebuilt = cot_chat.build_chat_cot(decoded, "urtn-x", "PEER",
                                          "2026-09-12T09:00:00.000Z")
        self.assertIn('uid1="%s"' % decoded["recipient"], rebuilt)


class SendTimeTests(unittest.TestCase):
    """A backlog replayed to somebody who was away is worth nothing if every
    line is stamped with the moment it was replayed."""

    def test_the_authors_time_is_carried(self):
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        self.assertEqual(cot_chat._iso(decoded["sent_unix"]), "2026-09-11T13:18:37.000Z")

    def test_a_replayed_line_keeps_its_own_time(self):
        decoded = cot_chat.decode(cot_chat.chat_from_cot(CHAT["message"], SENDER))
        rebuilt = cot_chat.build_chat_cot(decoded, "urtn-x", "PEER",
                                          "2026-09-12T09:00:00.000Z")
        self.assertIn('time="2026-09-11T13:18:37.000Z"', rebuilt)
        self.assertEqual(cot_chat.decode(cot_chat.chat_from_cot(rebuilt, SENDER))["sent_unix"],
                         decoded["sent_unix"])

    def test_an_event_with_no_time_says_so_rather_than_guessing(self):
        """Zero rather than now: a receiver can tell "not stated" from a time,
        and stamping the relay moment is what makes a backlog look simultaneous."""
        event = re.sub(r'\s*time="[^"]*"', '', CHAT["message"])
        decoded = cot_chat.decode(cot_chat.chat_from_cot(event, SENDER))
        self.assertEqual(decoded["sent_unix"], 0)


class PresenceTests(unittest.TestCase):
    """A peer with no endpoint appears on the map and is absent from the
    contact list, so nobody can chat with them, send them a marker, or
    dispatch them a CASEVAC."""

    def build(self, team="Cyan"):
        import cot_gateway
        import cot_position
        fix = cot_position.fix_from_cot(FIXTURES["tier2"]["cot"], SENDER)
        return cot_gateway.build_cot(fix, "urtn-" + "ab" * 16, "PEER", 120,
                                     team=team).decode("utf-8")

    def test_a_rendered_peer_is_addressable(self):
        self.assertIn('endpoint="*:-1:stcp"', self.build())

    def test_the_team_is_the_one_this_node_is_on(self):
        """Hard-coding Cyan put every peer in the wrong group on any other
        team, and group colour is how an operator tells their own people apart."""
        self.assertIn('name="Magenta"', self.build(team="Magenta"))




if __name__ == "__main__":
    unittest.main()
