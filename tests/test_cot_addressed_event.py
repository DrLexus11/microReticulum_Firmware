"""Anything ATAK sends to one contact reaches that contact, not the team.

Markers honoured `marti/dest` since PR D; the generic path did not. Found on
the bench 2026-09-22: a data package notice ATAK addressed to LEXUS reached
NEXUS too, and the absent test peer's copy was parked at the propagation node.
A drawing shared with one person would have gone the same way.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_coalesce                     # noqa: E402
from cot_bridge import CotBridge        # noqa: E402
from cot_endpoint import CotOutbound    # noqa: E402
from test_cot_tier3_bridge import big_event   # noqa: E402

LEXUS = b"\x11" * 16
NEXUS = b"\x22" * 16
ABSENT = b"\x33" * 16
CALLSIGNS = {LEXUS: "LEXUS", NEXUS: "NEXUS", ABSENT: "OLDPEER"}


def addressed(xml, *callsigns):
    dests = "".join('<dest callsign="%s"/>' % name for name in callsigns)
    return xml.replace("</detail>", "<marti>%s</marti></detail>" % dests)


def bridge():
    made = CotBridge.__new__(CotBridge)
    made.sent = made.fragmented = made.unreachable = made.unaddressable = 0
    made._in_flight = {}
    made._latest = cot_coalesce.LatestWins()
    made.pipeline = CotOutbound("urtn-" + "aa" * 16)
    made.registry = Mock()
    made.registry.members.return_value = [LEXUS, NEXUS, ABSENT]
    made.registry.describe.side_effect = lambda member: {"callsign": CALLSIGNS[member]}
    made.rns = Mock()
    made.rns.Identity.recall.side_effect = lambda member: member
    made.lxmf = Mock()
    made.to = []
    made.lxmf.send_frame.side_effect = lambda identity, frame: made.to.append(identity) or Mock()
    made._chat_from_atak = Mock(return_value=False)
    made._marker_from_atak = Mock(return_value=False)
    return made


def send(made, xml):
    with redirect_stdout(io.StringIO()):
        made._from_atak(xml)


class AddressedEventTests(unittest.TestCase):
    def test_a_broadcast_drawing_still_goes_to_everyone(self):
        made = bridge()
        send(made, big_event())
        self.assertEqual(set(made.to), {LEXUS, NEXUS, ABSENT})

    def test_a_drawing_sent_to_one_contact_goes_only_to_them(self):
        made = bridge()
        send(made, addressed(big_event(), "LEXUS"))
        self.assertTrue(made.to)
        self.assertEqual(set(made.to), {LEXUS})

    def test_sent_to_two_it_goes_to_those_two(self):
        made = bridge()
        send(made, addressed(big_event(), "LEXUS", "NEXUS"))
        self.assertEqual(set(made.to), {LEXUS, NEXUS})

    def test_addressed_to_nobody_on_the_team_is_refused_not_broadcast(self):
        made = bridge()
        send(made, addressed(big_event(), "SOMEONE-ELSE"))
        self.assertEqual(made.to, [])
        self.assertEqual(made.unaddressable, 1)

    def test_shared_with_one_then_another_both_receive_it(self):
        """Latest-wins is keyed on the addressees too: sharing the same drawing
        with a second person is not a newer version of the first share, and
        must neither be coalesced away nor withdraw the first one's delivery."""
        made = bridge()
        send(made, addressed(big_event(), "LEXUS"))
        send(made, addressed(big_event(), "NEXUS"))
        self.assertEqual(set(made.to), {LEXUS, NEXUS})
        made.lxmf.cancel.assert_not_called()

    def test_a_version_key_round_trips_its_addressees(self):
        key = CotBridge._version_key("DRAW-1", [NEXUS, LEXUS])
        self.assertEqual(sorted(CotBridge._recipients_in(key)), sorted([LEXUS, NEXUS]))
        self.assertIsNone(CotBridge._recipients_in(CotBridge._version_key("DRAW-1", None)))


if __name__ == "__main__":
    unittest.main()
