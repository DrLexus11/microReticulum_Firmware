"""An oversized event crossing the bridge, out and back.

Before this, anything over 383 B was refused outright, so a drawing or a
nine-line MEDEVAC was not late -- it was gone. These assert the two halves:
the send path cuts up rather than refuses, and the receive path puts the
pieces back before anything downstream sees them.
"""

import collections
import io
import sys
import threading
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import Mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_coalesce                   # noqa: E402
import cot_fragment                   # noqa: E402
import cot_tier2 as tier2             # noqa: E402
import tak_payload                    # noqa: E402
from cot_bridge import CotBridge      # noqa: E402
from cot_endpoint import CotOutbound  # noqa: E402

OUR_UID = "urtn-" + "aa" * 16


def big_event(points=40):
    """A drawing large enough that tier 2 alone refuses it.

    The coordinates are hash-derived rather than a tidy sequence, and that is
    not fussiness. Evenly spaced points deflate to almost nothing: the same
    forty points laid out regularly compress to a 368 B frame and sail under
    the bound, which would leave every test below asserting on a path that was
    never taken. Real drawings are not regular. This is the third fixture in
    this project to be caught compressing away the thing it was meant to
    measure.
    """
    import hashlib
    seq = [hashlib.sha256(str(i).encode()).hexdigest() for i in range(points)]
    links = "".join('<link point="41.%s,28.%s"/>' % (s[:4], s[4:8]) for s in seq)
    return ('<event version="2.0" uid="DRAW-1" type="u-d-f" how="h-g-i-g-o" '
            'time="2026-09-20T19:00:00.000Z" start="2026-09-20T19:00:00.000Z" '
            'stale="2026-09-20T20:00:00.000Z">'
            '<point lat="41.0" lon="28.9" hae="0" ce="9999999" le="9999999"/>'
            '<detail><contact callsign="LEXUS"/>%s'
            '<strokeColor value="-65536"/></detail></event>' % links)


class SendPathTests(unittest.TestCase):
    def setUp(self):
        self.pipeline = CotOutbound(OUR_UID)
        self.xml = big_event()

    def test_the_event_is_genuinely_too_big_for_one_packet(self):
        """If this ever stops raising, the fixture has drifted and the tests
        below stop testing anything."""
        with self.assertRaises(ValueError):
            tier2.encode(self.xml)

    def test_without_a_fragmenter_it_is_still_refused(self):
        """The old behaviour, kept deliberately: a caller that cannot carry
        fragments must not be handed them."""
        with redirect_stdout(io.StringIO()):
            self.assertEqual(self.pipeline.frames(self.xml, tier2.encode), [])

    def test_with_one_it_is_cut_up(self):
        with redirect_stdout(io.StringIO()):
            frames = self.pipeline.frames(self.xml, tier2.encode,
                                          cot_fragment.fragments)
        self.assertGreater(len(frames), 1)
        for frame in frames:
            self.assertLessEqual(len(frame), 383)
            self.assertEqual(tak_payload.kind_of(frame), tak_payload.FRAGMENT_V1)

    def test_an_ordinary_event_is_still_exactly_one_frame(self):
        """The common case pays nothing for the existence of the unusual one."""
        small = ('<event version="2.0" uid="M-1" type="a-f-G-U-C" how="m-g" '
                 'time="2026-09-20T19:00:00.000Z" start="2026-09-20T19:00:00.000Z" '
                 'stale="2026-09-20T19:05:00.000Z">'
                 '<point lat="41.0" lon="28.9" hae="0" ce="9999999" le="9999999"/>'
                 '<detail/></event>')
        frames = self.pipeline.frames(small, tier2.encode, cot_fragment.fragments)
        self.assertEqual(len(frames), 1)
        self.assertEqual(tak_payload.kind_of(frames[0]), tak_payload.COT_TIER2)

    def test_a_refusal_that_is_not_about_size_is_not_fragmented(self):
        """Malformed XML and our own echo both mean "do not send this".
        Fragmenting one would put the same refusal on the air in pieces."""
        with redirect_stdout(io.StringIO()):
            self.assertEqual(
                self.pipeline.frames("not xml at all", tier2.encode,
                                     cot_fragment.fragments), [])


class RoundTripTests(unittest.TestCase):
    def test_what_goes_out_in_pieces_comes_back_whole(self):
        pipeline = CotOutbound(OUR_UID)
        xml = big_event()
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(xml, tier2.encode, cot_fragment.fragments)
        reassembler = cot_fragment.Reassembler()
        whole = None
        for frame in frames:
            whole = reassembler.feed(b"\x99" * 16, frame)
        self.assertIsNotNone(whole, "fragments never reassembled")
        self.assertEqual(tak_payload.kind_of(whole), tak_payload.COT_TIER2)
        self.assertIn("DRAW-1", tier2.decode(whole))


class BridgeReceiveTests(unittest.TestCase):
    """The receive path hands a reassembled event on as though it had arrived
    in one piece, which is what keeps everything downstream unaware."""

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.clients = []
        made.clients_lock = threading.Lock()
        made._replay = collections.deque(maxlen=64)
        made._replay_lock = threading.Lock()
        made.replayed = 0
        made.received = 0
        made.unreadable = 0
        made.reassembled = 0
        # Built the way the bridge builds it, observer included -- a fixture
        # that wires this up differently from production tests a bridge that
        # does not exist.
        made._fragment_elapsed = 0.0
        made._reassembler = cot_fragment.Reassembler(
            observer=made._fragment_arrived)
        made._freshness = cot_coalesce.Freshness()
        made._heard_mesh = lambda: None
        made.drawn = []
        made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
        return made

    def test_a_partial_transfer_draws_nothing(self):
        made = self.bridge()
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(big_event(), tier2.encode, cot_fragment.fragments)
        packet = Mock()
        packet.link = None
        packet.destination_hash = b"\x77" * 16
        with redirect_stdout(io.StringIO()):
            made._from_mesh(frames[0], packet)
        self.assertEqual(made.drawn, [])

    def test_the_last_fragment_delivers_the_whole_event(self):
        made = self.bridge()
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(big_event(), tier2.encode, cot_fragment.fragments)
        packet = Mock()
        packet.link = None
        packet.destination_hash = b"\x77" * 16
        with redirect_stdout(io.StringIO()):
            for frame in frames:
                made._from_mesh(frame, packet)
        self.assertEqual(len(made.drawn), 1)
        self.assertIn(b"DRAW-1", made.drawn[0])
        self.assertEqual(made.reassembled, 1)


class CarriedReliablyTests(unittest.TestCase):
    """How a fragment travels, which is the whole of whether tier 3 works.

    A bare packet has no proof, no retry and no propagation node. Measured on
    hardware 2026-09-21, a two-fragment drawing lost its second fragment and
    nothing retried it: the far end held half a drawing until the reassembly
    window expired. The design always said each fragment is a whole LXMF
    message; these are what say it actually is one.
    """

    def bridge(self, lxmf=None, recall=True):
        made = CotBridge.__new__(CotBridge)
        made.lxmf = lxmf
        made.unreachable = 0
        made.sent = 0
        made.registry = Mock()
        made.registry.members.return_value = [b"\x11" * 16, b"\x22" * 16]
        made.rns = Mock()
        made.rns.Identity.recall.return_value = Mock() if recall else None
        made.packets = []
        made._send_to = lambda member, frame: made.packets.append(
            (member, frame)) or 1
        return made

    def test_a_fragment_goes_over_lxmf_not_as_a_bare_packet(self):
        carrier = Mock()
        carrier.send_frame.return_value = True
        made = self.bridge(lxmf=carrier)

        self.assertEqual(made._fan_out_reliably(b"\x05frag"), 2)
        self.assertEqual(carrier.send_frame.call_count, 2)
        self.assertEqual(made.packets, [], "a fragment went out unproved")

    def test_lxmf_declining_does_not_fall_back_to_a_bare_packet(self):
        """Sending unreliably while the caller believes it sent reliably is
        worse than not sending. Same doctrine as an addressed chat line."""
        carrier = Mock()
        carrier.send_frame.return_value = None
        made = self.bridge(lxmf=carrier)

        self.assertEqual(made._fan_out_reliably(b"\x05frag"), 0)
        self.assertEqual(made.packets, [])
        self.assertEqual(made.unreachable, 2)

    def test_without_lxmf_the_bare_packet_is_still_the_route(self):
        """--no-lxmf is how somebody chooses best effort. It is not something
        to hand them by accident, and it is not nothing either."""
        made = self.bridge(lxmf=None)

        self.assertEqual(made._fan_out_reliably(b"\x05frag"), 2)
        self.assertEqual(len(made.packets), 2)

    def test_a_peer_whose_identity_is_lost_still_gets_a_packet(self):
        carrier = Mock()
        made = self.bridge(lxmf=carrier, recall=False)

        self.assertEqual(made._fan_out_reliably(b"\x05frag"), 2)
        self.assertEqual(len(made.packets), 2)
        carrier.send_frame.assert_not_called()


class AutoSendTests(unittest.TestCase):
    """Several versions of one event in quick succession.

    One version of a drawing costs about 7.7 s of channel for a team of seven,
    so a drag that emits a version a second would ask for eight times what the
    channel has. The coalescing itself is pinned in test_cot_coalesce; these
    pin how the bridge uses it.
    """

    def frames(self, xml):
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            return pipeline.frames(xml, tier2.encode, cot_fragment.fragments)

    def test_the_same_drawing_cut_up_twice_has_one_digest(self):
        """Every transfer draws a random transfer id into its headers, so
        hashing the fragments never matched and an unchanged repeat was never
        recognised. The first version of this gate had exactly that bug."""
        xml = big_event()
        first, second = self.frames(xml), self.frames(xml)
        self.assertNotEqual(first, second, "transfer ids stopped being random")

        self.assertEqual(CotBridge._content_digest(first),
                         CotBridge._content_digest(second))

    def test_a_changed_drawing_has_a_different_digest(self):
        self.assertNotEqual(
            CotBridge._content_digest(self.frames(big_event())),
            CotBridge._content_digest(self.frames(big_event(points=41))))

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.sent = 0
        made.fragmented = 0
        made.unreachable = 0
        made._in_flight = {}
        made.lxmf = Mock()
        made.lxmf.send_frame.side_effect = lambda identity, frame: Mock()
        made.registry = Mock()
        made.registry.members.return_value = [b"\x11" * 16]
        made.rns = Mock()
        return made

    def test_a_new_version_withdraws_what_is_left_of_the_last(self):
        """A version still retrying after a newer one has gone is airtime spent
        delivering a shape the operator already moved."""
        made = self.bridge()
        frames = self.frames(big_event())
        with redirect_stdout(io.StringIO()):
            made._send_version("DRAW-1", frames)
            first = list(made._in_flight["DRAW-1"])
            made._send_version("DRAW-1", self.frames(big_event(points=41)))

        self.assertEqual(len(first), len(frames))
        self.assertEqual(made.lxmf.cancel.call_count, len(first))
        for message in first:
            made.lxmf.cancel.assert_any_call(message)

    def test_a_different_drawing_withdraws_nothing(self):
        made = self.bridge()
        with redirect_stdout(io.StringIO()):
            made._send_version("DRAW-1", self.frames(big_event()))
            made._send_version("DRAW-2", self.frames(big_event(points=41)))
        made.lxmf.cancel.assert_not_called()

    def test_a_single_packet_version_takes_the_cheap_fan_out(self):
        made = self.bridge()
        made._fan_out = Mock(return_value=1)
        made._send_version("SMALL", [b"\x01one"])
        made._fan_out.assert_called_once_with(b"\x01one")
        made.lxmf.send_frame.assert_not_called()


class FreshnessOnReceiveTests(unittest.TestCase):
    """An older version finishing its retries after a newer one landed."""

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made._freshness = cot_coalesce.Freshness()
        return made

    def event(self, when):
        return ('<event version="2.0" uid="DRAW-1" type="u-d-f" time="%s" '
                'start="%s" stale="2026-09-21T09:00:00Z"/>' % (when, when))

    def test_a_late_older_version_is_not_drawn(self):
        made = self.bridge()
        with redirect_stdout(io.StringIO()):
            self.assertTrue(made._is_fresh(self.event("2026-09-21T07:00:05Z")))
            self.assertFalse(made._is_fresh(self.event("2026-09-21T07:00:00Z")))

    def test_a_newer_version_is_drawn(self):
        made = self.bridge()
        made._is_fresh(self.event("2026-09-21T07:00:00Z"))
        self.assertTrue(made._is_fresh(self.event("2026-09-21T07:00:05Z")))

    def test_malformed_xml_is_not_this_checks_business(self):
        self.assertTrue(self.bridge()._is_fresh("<not xml"))


class FragmentsOverLxmfArriveTests(unittest.TestCase):
    """A fragment that came by LXMF has to be put back together too.

    Moving the send to LXMF moves the receive with it: fragments no longer
    arrive on the packet callback at all. A receive path that still only
    understood packets would have turned one silent failure into another.
    """

    def bridge(self):
        made = CotBridge.__new__(CotBridge)
        made.clients = []
        made.clients_lock = threading.Lock()
        made._replay = collections.deque(maxlen=64)
        made._replay_lock = threading.Lock()
        made.replayed = 0
        made.received = 0
        made.unreadable = 0
        made.reassembled = 0
        made.registry = Mock()
        made._fragment_elapsed = 0.0
        made._reassembler = cot_fragment.Reassembler(
            observer=made._fragment_arrived)
        made._freshness = cot_coalesce.Freshness()
        made._member_for_lxmf = lambda source: b"\x42" * 16
        made.drawn = []
        made._to_clients = lambda payload=None, keep=False: made.drawn.append(payload)
        return made

    def test_a_drawing_split_over_lxmf_is_rejoined(self):
        made = self.bridge()
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(big_event(), tier2.encode, cot_fragment.fragments)
        self.assertGreater(len(frames), 1, "fixture is no longer fragmented")

        with redirect_stdout(io.StringIO()):
            for frame in frames:
                made._chat_from_lxmf(frame, b"\x77" * 16)

        self.assertEqual(len(made.drawn), 1)
        self.assertIn(b"DRAW-1", made.drawn[0])
        self.assertEqual(made.reassembled, 1)

    def test_a_sender_that_resolves_only_sometimes_still_completes(self):
        """Recall can fail for one message and succeed for the next.

        Keyed on the resolved member, fragments 1 and 3 of a transfer landed
        under one key and fragment 2 under another, and the drawing never
        completed -- measured on the bench. The source hash does not change
        between one message and the next.
        """
        made = self.bridge()
        answers = iter([b"\x42" * 16, None, b"\x42" * 16, None])
        made._member_for_lxmf = lambda source: next(answers, None)
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(big_event(), tier2.encode, cot_fragment.fragments)
            for frame in frames:
                made._chat_from_lxmf(frame, b"\x77" * 16)

        self.assertEqual(made.reassembled, 1)
        self.assertEqual(len(made.drawn), 1)

    def test_a_partial_transfer_over_lxmf_draws_nothing(self):
        made = self.bridge()
        pipeline = CotOutbound(OUR_UID)
        with redirect_stdout(io.StringIO()):
            frames = pipeline.frames(big_event(), tier2.encode, cot_fragment.fragments)

        with redirect_stdout(io.StringIO()):
            made._chat_from_lxmf(frames[0], b"\x77" * 16)

        self.assertEqual(made.drawn, [])


if __name__ == "__main__":
    unittest.main()
