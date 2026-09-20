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
        made._reassembler = cot_fragment.Reassembler()
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


if __name__ == "__main__":
    unittest.main()
