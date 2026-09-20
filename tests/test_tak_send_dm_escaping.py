"""Every value from a command line, escaped before it becomes XML.

The codecs in cot_chat.py and cot_marker.py have always done this. This helper
escaped only the message text, which is the value least likely to be a problem
-- a callsign with an ampersand in it is ordinary, and it closed an attribute
early.
"""

import sys
import unittest
from datetime import datetime, timezone
from pathlib import Path
from xml.etree import ElementTree

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tak_send_dm  # noqa: E402

NASTY = 'A&B "quoted" <tag>'


class EscapingTests(unittest.TestCase):
    def test_a_hostile_callsign_still_parses(self):
        xml = tak_send_dm.build(
            sender_uid="urtn-1111", recipient_uid="urtn-2222",
            callsign=NASTY, room="All Chat Rooms", text="hello",
            message_id="abc", now=datetime.now(timezone.utc))
        ElementTree.fromstring(xml)     # must not raise

    def test_the_callsign_survives_intact(self):
        """Escaped, not stripped. An operator whose callsign has an ampersand
        in it keeps the ampersand."""
        xml = tak_send_dm.build(
            sender_uid="urtn-1111", recipient_uid="urtn-2222",
            callsign=NASTY, room="Room", text="hello",
            message_id="abc", now=datetime.now(timezone.utc))
        event = ElementTree.fromstring(xml)
        chat = event.find("detail/__chat")
        self.assertEqual(chat.get("senderCallsign"), NASTY)

    def test_a_hostile_room_cannot_add_an_attribute(self):
        """The injection this prevents: a value that closes its own attribute
        and opens another."""
        xml = tak_send_dm.build(
            sender_uid="urtn-1111", recipient_uid="urtn-2222",
            callsign="DECK", room='X" groupOwner="true', text="hello",
            message_id="abc", now=datetime.now(timezone.utc))
        event = ElementTree.fromstring(xml)
        chat = event.find("detail/__chat")
        self.assertEqual(chat.get("groupOwner"), "false")
        self.assertEqual(chat.get("chatroom"), 'X" groupOwner="true')

    def test_the_text_is_still_escaped(self):
        xml = tak_send_dm.build(
            sender_uid="urtn-1111", recipient_uid="urtn-2222",
            callsign="DECK", room="Room", text=NASTY,
            message_id="abc", now=datetime.now(timezone.utc))
        event = ElementTree.fromstring(xml)
        self.assertEqual(event.find("detail/remarks").text, NASTY)


if __name__ == "__main__":
    unittest.main()
