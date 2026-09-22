"""A marker sent to one person reaches that person, and nobody else.

ATAK's "Send" to a contact puts the recipient in `detail/marti/dest`. Before
this the bridge ignored it and fanned every marker out to the whole team: a pin
dropped for one person was shown to everybody -- the confidentiality problem
the chat path was built to avoid, on a different codec.

No wire change was needed. Markers already go out one unicast packet per
member, so an addressed marker is simply sent only to its addressee; the
receiver draws whatever arrives. What changed is the dispatch, and that an
addressed marker travels over LXMF so it is proved like a chat line.
"""

import io
import sys
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_marker                      # noqa: E402
import tak_identity                    # noqa: E402
from cot_bridge import CotBridge       # noqa: E402

LEXUS = b"\x11" * 16
BRAVO = b"\x22" * 16
OUR_SENDER_ID = 0x0A0B0C0D


def marker(dest=None):
    """A hostile marker, broadcast unless `dest` names a recipient."""
    marti = ""
    if dest is not None:
        attrs = " ".join('%s="%s"' % item for item in dest.items())
        marti = "<marti><dest %s/></marti>" % attrs
    return ('<event version="2.0" uid="c4f2e1a0-1111-2222-3333-444455556666" '
            'type="a-h-G" how="h-g-i-g-o" time="2026-09-21T10:30:00Z" '
            'start="2026-09-21T10:30:00Z" stale="2026-09-22T10:30:00Z">'
            '<point lat="41.0151234" lon="28.9791234" hae="12" ce="9999999" le="9999999"/>'
            '<detail><contact callsign="Rubble 3"/>%s</detail></event>' % marti)


class SendTests(unittest.TestCase):
    def bridge(self, lxmf=True):
        made = CotBridge.__new__(CotBridge)
        made.sender_id = OUR_SENDER_ID
        made.markers_sent = 0
        made.markers_undeliverable = 0
        made.spi_gate = Mock()
        made.registry = Mock()
        made.registry.members.return_value = [LEXUS, BRAVO]
        made.registry.describe.side_effect = lambda member: {
            LEXUS: {"callsign": "LEXUS"}, BRAVO: {"callsign": "BRAVO"}}.get(member)
        made.rns = Mock()
        made.rns.Identity.recall.side_effect = lambda member: "identity-of-%s" % member.hex()[:4]
        made.lxmf = Mock() if lxmf else None
        if lxmf:
            made.lxmf.send_frame.return_value = Mock()
        made._fan_out = Mock(return_value=2)
        made._send_to = Mock(return_value=1)
        return made

    def send(self, made, xml):
        with redirect_stdout(io.StringIO()):
            return made._marker_from_atak(xml)

    def test_a_broadcast_marker_still_goes_to_everyone(self):
        made = self.bridge()
        self.assertTrue(self.send(made, marker()))
        made._fan_out.assert_called_once()
        made.lxmf.send_frame.assert_not_called()

    def test_a_marker_sent_by_callsign_goes_only_to_that_member(self):
        made = self.bridge()
        self.send(made, marker({"callsign": "LEXUS"}))

        made._fan_out.assert_not_called()
        made.lxmf.send_frame.assert_called_once()
        identity, frame = made.lxmf.send_frame.call_args[0]
        self.assertEqual(identity, "identity-of-1111")
        self.assertEqual(cot_marker.decode(frame)["type"], "a-h-G")

    def test_a_marker_sent_by_uid_goes_only_to_that_member(self):
        made = self.bridge()
        self.send(made, marker({"uid": tak_identity.uid_for(BRAVO), "callsign": "BRAVO"}))
        made.lxmf.send_frame.assert_called_once()
        self.assertEqual(made.lxmf.send_frame.call_args[0][0], "identity-of-2222")

    def test_an_addressee_nobody_on_the_team_carries_is_not_broadcast_instead(self):
        """Broadcasting it would deliver a private pin to everyone, and would
        not reach the person it was meant for either."""
        made = self.bridge()
        self.send(made, marker({"callsign": "SOMEONE-ELSE"}))

        made._fan_out.assert_not_called()
        made.lxmf.send_frame.assert_not_called()
        self.assertEqual(made.markers_undeliverable, 1)

    def test_lxmf_declining_is_counted_not_downgraded_to_a_packet(self):
        made = self.bridge()
        made.lxmf.send_frame.return_value = None
        self.send(made, marker({"callsign": "LEXUS"}))

        made._send_to.assert_not_called()
        self.assertEqual(made.markers_undeliverable, 1)

    def test_without_lxmf_an_addressed_marker_still_goes_only_to_its_addressee(self):
        made = self.bridge(lxmf=False)
        self.send(made, marker({"callsign": "LEXUS"}))

        made._fan_out.assert_not_called()
        made._send_to.assert_called_once()
        self.assertEqual(made._send_to.call_args[0][0], LEXUS)


class ReceiveTests(unittest.TestCase):
    """A marker that came addressed arrives over LXMF, where the carrier says
    who sent it -- and the claim in the frame must agree."""

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.unreadable = 0
        made.received = 0
        made.markers_received = 0
        made.registry = Mock()
        made.registry.resolve_sender_id.return_value = LEXUS
        made.registry.describe.return_value = {"callsign": "LEXUS"}
        made._member_for_lxmf = lambda source: made.proved
        made.drawn = []
        made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
        return made

    def frame(self):
        return cot_marker.marker_from_cot(marker(), 0x11111111)

    def test_a_proved_marker_is_drawn(self):
        made = self.bridge()
        made.proved = LEXUS
        with redirect_stdout(io.StringIO()):
            made._chat_from_lxmf(self.frame(), b"\x77" * 16)
        self.assertEqual(len(made.drawn), 1)
        self.assertIn(b"a-h-G", made.drawn[0])

    def test_a_marker_whose_claim_disagrees_with_its_proof_is_not_drawn(self):
        """Otherwise any member could drop a pin under another's name."""
        made = self.bridge()
        made.proved = BRAVO
        with redirect_stdout(io.StringIO()):
            made._chat_from_lxmf(self.frame(), b"\x77" * 16)
        self.assertEqual(made.drawn, [])
        self.assertEqual(made.unreadable, 1)


class HeldMarkerKeepsItsProofTests(unittest.TestCase):
    """A marker held until its sender was known comes back through
    _dispatch_frame; the carrier proof must come back with it."""

    def test_a_released_marker_whose_proof_disagrees_is_not_drawn(self):
        made = ReceiveTests.bridge(ReceiveTests())
        frame = cot_marker.marker_from_cot(marker(), 0x11111111)
        with redirect_stdout(io.StringIO()):
            made._dispatch_frame(frame, signed_by=BRAVO)     # claims LEXUS
        self.assertEqual(made.drawn, [])

    def test_a_released_marker_whose_proof_agrees_is_drawn(self):
        made = ReceiveTests.bridge(ReceiveTests())
        frame = cot_marker.marker_from_cot(marker(), 0x11111111)
        with redirect_stdout(io.StringIO()):
            made._dispatch_frame(frame, signed_by=LEXUS)
        self.assertEqual(len(made.drawn), 1)


if __name__ == "__main__":
    unittest.main()
