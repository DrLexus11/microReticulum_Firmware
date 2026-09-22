"""Cutting an oversized event into packets that each stand on their own.

Tier 2 refuses above 383 B, which is why drawings and a nine-line MEDEVAC do
not cross. Each fragment here is a whole LXMF message, so LXMF's own proof and
retry give selective retransmission and the far end only reassembles. The shape
was chosen by measurement: below roughly six fragments the link handshake a
Resource needs costs more than per-fragment proofs, and everything on the LoRa
requirement list is below that.
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_fragment                   # noqa: E402
import tak_payload                    # noqa: E402

PEER = b"\x11" * 16
OTHER = b"\x22" * 16


class WireTests(unittest.TestCase):
    def test_every_fragment_fits_one_packet(self):
        """The whole reason this exists. A fragment that needed fragmenting
        would be an infinite regress."""
        for frame in cot_fragment.fragments(b"x" * 2000):
            self.assertLessEqual(len(frame), 383)

    def test_a_fragment_announces_itself_in_byte_zero(self):
        """One namespace shared by every codec on this destination. A frame
        read as the wrong kind does not fail cleanly -- it decodes into
        something nobody sent."""
        frame = cot_fragment.fragments(b"x" * 100)[0]
        self.assertEqual(tak_payload.kind_of(frame), tak_payload.FRAGMENT_V1)

    def test_a_payload_that_already_fits_is_still_one_fragment(self):
        """Not special-cased. A receiver that only understands fragments is
        simpler than one that must also recognise an unfragmented form."""
        frames = cot_fragment.fragments(b"x" * 50)
        self.assertEqual(len(frames), 1)
        self.assertEqual(cot_fragment.decode(frames[0])[2], 1)

    def test_a_drawing_is_three_fragments(self):
        """A compressed drawing is about 700 B, and that is three fragments.

        It was two while a fragment was sized against the 383 B bare-packet
        MDU. A fragment does not travel as a bare packet -- it travels as an
        LXMF message, for the proof and the retry -- and the envelope costs a
        measured 107 bytes, so the slice had to come down. Three is still well
        inside the crossover where a Resource would be the better scheme.
        """
        self.assertEqual(len(cot_fragment.fragments(b"x" * 700)), 3)

    def test_a_fragment_fits_one_lxmf_message_in_one_packet(self):
        """The bound that matters, and the one that was wrong.

        A fragment travels as an LXMF message, not as a bare packet, so 383 is
        not its budget: the envelope costs a measured 107 bytes on the air. A
        frame sized against 383 packs to 490 and LXMF builds a Resource over a
        Link for it -- one link per fragment, on a path where establishment was
        5 of 15 when busy. That is worse than the single Resource this scheme
        exists to avoid, and it would have looked like tier 3 working.
        """
        biggest = max(cot_fragment.fragments(b"x" * 5000), key=len)
        on_the_air = len(biggest) + cot_fragment.LXMF_ENVELOPE_BYTES

        self.assertLessEqual(on_the_air, 383)
        # And with the headroom for fields a message may yet carry.
        self.assertLessEqual(len(biggest), cot_fragment.MAX_FRAGMENT_FRAME_BYTES)

    def test_nothing_is_not_a_transfer(self):
        with self.assertRaises(ValueError):
            cot_fragment.fragments(b"")

    def test_a_payload_beyond_the_bound_says_where_it_belongs(self):
        """Above the crossover this scheme is the wrong one, and the error
        should say so rather than just refusing."""
        too_big = b"x" * (cot_fragment.MAX_FRAGMENT_BYTES * 256)
        with self.assertRaises(ValueError) as caught:
            cot_fragment.fragments(too_big)
        self.assertIn("Resource", str(caught.exception))


class DecodeTests(unittest.TestCase):
    def test_another_codec_is_not_ours(self):
        """Every codec shares this destination. A frame that is not ours is
        ordinary, not an error."""
        self.assertIsNone(cot_fragment.decode(bytes([tak_payload.CHAT_V1]) + b"x" * 40))

    def test_rubbish_is_refused_rather_than_unpacked(self):
        for bad in (b"", b"\x05", b"\x05\x00\x00", None, "not bytes"):
            self.assertIsNone(cot_fragment.decode(bad))

    def test_a_count_of_zero_can_never_be_satisfied(self):
        """A buffer held for a transfer that cannot complete is how a
        malformed peer costs us memory."""
        frame = cot_fragment.HEADER.pack(tak_payload.FRAGMENT_V1, 1234, 0, 0) + b"x"
        self.assertIsNone(cot_fragment.decode(frame))

    def test_an_index_outside_the_count_is_refused(self):
        frame = cot_fragment.HEADER.pack(tak_payload.FRAGMENT_V1, 1234, 5, 3) + b"x"
        self.assertIsNone(cot_fragment.decode(frame))


class ReassemblyTests(unittest.TestCase):
    def setUp(self):
        self.reassembler = cot_fragment.Reassembler()

    def test_a_transfer_comes_back_whole(self):
        payload = bytes(range(256)) * 4
        out = None
        for frame in cot_fragment.fragments(payload):
            out = self.reassembler.feed(PEER, frame)
        self.assertEqual(out, payload)
        self.assertEqual(self.reassembler.pending(), 0)

    def test_fragments_may_arrive_in_any_order(self):
        """They are separate LXMF messages with separate retries, so order is
        not something this can assume."""
        payload = b"abcdefgh" * 200
        frames = cot_fragment.fragments(payload)
        out = None
        for frame in reversed(frames):
            out = self.reassembler.feed(PEER, frame)
        self.assertEqual(out, payload)

    def test_nothing_is_returned_until_every_fragment_is_in(self):
        frames = cot_fragment.fragments(b"x" * 1500)
        for frame in frames[:-1]:
            self.assertIsNone(self.reassembler.feed(PEER, frame))
        self.assertIsNotNone(self.reassembler.feed(PEER, frames[-1]))

    def test_two_peers_may_pick_the_same_transfer_id(self):
        """Keyed on the sender as well. Merging two peers' fragments would
        produce a frame that decodes to something neither of them sent, which
        is worse than dropping both."""
        mine = cot_fragment.fragments(b"A" * 700, transfer_id=7)
        theirs = cot_fragment.fragments(b"B" * 700, transfer_id=7)
        for index in range(len(mine) - 1):
            self.assertIsNone(self.reassembler.feed(PEER, mine[index]))
            self.assertIsNone(self.reassembler.feed(OTHER, theirs[index]))
        self.assertEqual(self.reassembler.feed(PEER, mine[-1]), b"A" * 700)
        self.assertEqual(self.reassembler.feed(OTHER, theirs[-1]), b"B" * 700)

    def test_a_stale_transfer_is_dropped(self):
        """A partial drawing is worth nothing, so holding one longer buys
        nothing either."""
        frames = cot_fragment.fragments(b"x" * 1500)
        self.reassembler.feed(PEER, frames[0], now=1000)
        self.assertIsNone(
            self.reassembler.feed(PEER, frames[1],
                                  now=1000 + cot_fragment.REASSEMBLY_TIMEOUT_SECONDS + 1))
        self.assertEqual(self.reassembler.pending(), 1)

    def test_a_peer_that_never_finishes_cannot_grow_the_buffer(self):
        reassembler = cot_fragment.Reassembler(max_transfers=4)
        for transfer in range(20):
            frame = cot_fragment.fragments(b"x" * 1500, transfer_id=transfer)[0]
            reassembler.feed(PEER, frame)
        self.assertLessEqual(reassembler.pending(), 4)

    def test_one_id_describing_two_lengths_is_dropped(self):
        """One of the two is not what it claims, and neither is worth guessing
        at."""
        first = cot_fragment.fragments(b"x" * 700, transfer_id=9)[0]
        conflicting = cot_fragment.fragments(b"y" * 1500, transfer_id=9)[0]
        self.reassembler.feed(PEER, first)
        self.assertIsNone(self.reassembler.feed(PEER, conflicting))
        self.assertEqual(self.reassembler.pending(), 0)


class ObserverTests(unittest.TestCase):
    """The seam that lets a slow transfer be watched while it is still slow."""

    def setUp(self):
        self.seen = []
        self.reassembler = cot_fragment.Reassembler(
            observer=lambda *args: self.seen.append(args))

    def test_every_fragment_is_reported_including_the_last(self):
        """The last one closes the transfer, and it is the one that carries the
        elapsed time worth reading."""
        frames = cot_fragment.fragments(b"x" * 900, transfer_id=1)
        for frame in frames:
            self.reassembler.feed(PEER, frame)
        self.assertEqual(len(self.seen), len(frames))
        self.assertEqual([index for index, _, _, _ in self.seen],
                         list(range(len(frames))))

    def test_elapsed_is_measured_from_the_first_fragment(self):
        """Not from the previous one. The timeout is measured against the first,
        so a diagnostic that reported gaps would be answering a question nobody
        asked."""
        frames = cot_fragment.fragments(b"x" * 900, transfer_id=2)
        self.reassembler.feed(PEER, frames[0], now=1000.0)
        self.reassembler.feed(PEER, frames[1], now=1010.0)
        self.assertEqual(self.seen[0][3], 0.0)
        self.assertEqual(self.seen[1][3], 10.0)

    def test_held_counts_what_is_in_hand_not_what_is_expected(self):
        frames = cot_fragment.fragments(b"x" * 900, transfer_id=3)
        self.reassembler.feed(PEER, frames[1])
        self.assertEqual(self.seen[0][2], 1)

    def test_a_reassembler_without_an_observer_still_works(self):
        """The default path is the one that runs in the field."""
        frames = cot_fragment.fragments(b"x" * 900, transfer_id=4)
        plain = cot_fragment.Reassembler()
        whole = None
        for frame in frames:
            whole = plain.feed(PEER, frame) or whole
        self.assertEqual(whole, b"x" * 900)


class SharedFixtureTests(unittest.TestCase):
    """The byte-level vectors both repositories read.

    tak_native_v1.json is copied into columba and asserted on there. Until this
    class the Python suite never read its fragment block -- it generated its own
    vectors -- so the fixture could drift from cot_fragment.fragments with no
    Python test failing, and the cross-language contract would have been kept
    by one side only.
    """

    @classmethod
    def setUpClass(cls):
        import json
        path = Path(__file__).resolve().parent / "fixtures" / "tak_native_v1.json"
        cls.block = json.loads(path.read_text())["fragment"]

    def test_the_limits_are_the_ones_the_code_uses(self):
        self.assertEqual(self.block["kind"], tak_payload.FRAGMENT_V1)
        self.assertEqual(self.block["header_bytes"], cot_fragment.HEADER_BYTES)
        self.assertEqual(self.block["max_fragment_bytes"], cot_fragment.MAX_FRAGMENT_BYTES)
        self.assertEqual(self.block["max_fragment_frame_bytes"], cot_fragment.MAX_FRAGMENT_FRAME_BYTES)
        self.assertEqual(self.block["max_fragments"], cot_fragment.MAX_FRAGMENTS)
        self.assertEqual(self.block["reassembly_timeout_seconds"],
                         cot_fragment.REASSEMBLY_TIMEOUT_SECONDS)

    def test_cutting_the_fixture_payload_yields_the_fixture_frames(self):
        payload = bytes.fromhex(self.block["payload"])
        frames = cot_fragment.fragments(payload, transfer_id=self.block["transfer_id"])
        self.assertEqual([frame.hex() for frame in frames], self.block["frames"])

    def test_the_fixture_frames_reassemble_to_the_fixture_payload(self):
        reassembler = cot_fragment.Reassembler()
        whole = None
        for hex_frame in self.block["frames"]:
            whole = reassembler.feed(PEER, bytes.fromhex(hex_frame)) or whole
        self.assertEqual(whole, bytes.fromhex(self.block["payload"]))


class HostileFrameTests(unittest.TestCase):
    def test_a_frame_above_the_wire_maximum_is_not_a_fragment(self):
        """Over LXMF one message can be any size. Without this a malformed
        count-1 fragment had the reassembler hold a slice of any size."""
        header = cot_fragment.HEADER.pack(tak_payload.FRAGMENT_V1, 1, 0, 1)
        huge = header + b"x" * (cot_fragment.MAX_FRAGMENT_FRAME_BYTES + 1 - len(header))
        self.assertIsNone(cot_fragment.decode(huge))
        self.assertIsNone(cot_fragment.Reassembler().feed(PEER, huge))

    def test_a_frame_at_the_wire_maximum_still_is(self):
        header = cot_fragment.HEADER.pack(tak_payload.FRAGMENT_V1, 1, 0, 1)
        largest = header + b"x" * (cot_fragment.MAX_FRAGMENT_FRAME_BYTES - len(header))
        self.assertIsNotNone(cot_fragment.decode(largest))


class ConcurrentFeedTests(unittest.TestCase):
    def test_two_callback_threads_feeding_at_once_lose_nothing(self):
        """Reticulum's packet callback and LXMF's delivery callback both feed
        the reassembler. Unsynchronised, a fragment, an expiry and a completion
        racing on the same dictionary lost entries or raised."""
        import threading
        reassembler = cot_fragment.Reassembler(max_transfers=10_000)
        transfers = [cot_fragment.fragments(bytes([n % 256]) * 900, transfer_id=n)
                     for n in range(300)]
        completed, errors = [], []

        def feed(indices):
            try:
                for n in indices:
                    for frame in transfers[n]:
                        whole = reassembler.feed(PEER, frame)
                        if whole is not None:
                            completed.append(n)
            except Exception as error:                 # noqa: BLE001
                errors.append(error)

        workers = [threading.Thread(target=feed, args=(range(k, 300, 4),)) for k in range(4)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join()

        self.assertEqual(errors, [])
        self.assertEqual(sorted(completed), list(range(300)))
        self.assertEqual(reassembler.pending(), 0)


if __name__ == "__main__":
    unittest.main()
