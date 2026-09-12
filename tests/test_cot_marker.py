"""Point markers, the last of the 850 events PR B gives a typed codec."""

import json
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_marker
import cot_tier2
import tak_payload

FIXTURES = json.loads(
    (Path(__file__).resolve().parents[1] / "tests/fixtures/tak_native_v1.json").read_text())
MARKER = FIXTURES["marker"]
SENDER = 0x11223344
OURS = "urtn-" + "ab" * 16


class ClassificationTests(unittest.TestCase):
    def test_every_captured_marker_type_is_recognised(self):
        for kind in ("a-h-G", "b-m-p-s-m", "b-m-p-c-cp", "b-m-p-s-p-i"):
            self.assertTrue(cot_marker.is_marker(MARKER[kind]), kind)

    def test_a_self_report_is_not_a_marker(self):
        """It describes the reporter and belongs to the position codec, which
        has its own cadence. Sending it twice would double the airtime for the
        commonest event there is."""
        self.assertFalse(cot_marker.is_marker(FIXTURES["tier2"]["cot"]))

    def test_a_drawing_is_left_for_pr_c(self):
        drawing = ('<event uid="d1" type="u-d-f" how="h-e"><point lat="1" lon="2"/>'
                   '<detail><shape/></detail></event>')
        self.assertFalse(cot_marker.is_marker(drawing))

    def test_rubbish_is_not_a_marker(self):
        for bad in ("", "<event", "not xml", "<other/>"):
            self.assertFalse(cot_marker.is_marker(bad), bad)


class RealEventTests(unittest.TestCase):
    def test_every_captured_marker_shrinks_by_an_order_of_magnitude(self):
        for kind in ("a-h-G", "b-m-p-s-m", "b-m-p-c-cp", "b-m-p-s-p-i"):
            frame = cot_marker.marker_from_cot(MARKER[kind], SENDER)
            self.assertIsNotNone(frame, kind)
            self.assertLess(len(frame), len(MARKER[kind]) / 8, kind)

    def test_the_frames_match_the_stored_vectors(self):
        for kind in ("a-h-G", "b-m-p-s-m", "b-m-p-c-cp", "b-m-p-s-p-i"):
            self.assertEqual(cot_marker.marker_from_cot(MARKER[kind], SENDER).hex(),
                             MARKER[kind + "_frame"], kind)

    def test_a_hostile_marker_keeps_what_an_operator_sees(self):
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER))
        self.assertEqual(decoded["type"], "a-h-G")
        self.assertEqual(decoded["callsign"], "R.11.095131")
        self.assertEqual(decoded["lat_e7"], 410235622)
        self.assertEqual(decoded["stale_seconds"], 300)

    def test_a_marker_keeps_its_own_uid(self):
        """A marker is a distinct object. Collapsing them would put every
        marker a node ever dropped on one track."""
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["b-m-p-s-m"], SENDER))
        self.assertEqual(decoded["uid"], "cc5434e0-5d5e-48b4-b32a-29c7907dbfad")
        self.assertIsNone(decoded["uid_suffix"])


class SpiTests(unittest.TestCase):
    """SPI is the volume case -- 121 of the captured events -- and the one that
    leaks: ATAK names it after the sending device."""

    def test_the_senders_device_id_does_not_travel(self):
        frame = cot_marker.marker_from_cot(MARKER["b-m-p-s-p-i"], SENDER)
        self.assertNotIn(b"ANDROID", frame)
        self.assertNotIn(b"3f5c91a27be4d068", frame)

    def test_only_the_suffix_travels(self):
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["b-m-p-s-p-i"], SENDER))
        self.assertIsNone(decoded["uid"])
        self.assertEqual(decoded["uid_suffix"], "SPI1")

    def test_the_uid_is_rebuilt_against_the_reticulum_identity(self):
        """Which is pivot 1 applied to the one event type that had smuggled a
        device identifier past it."""
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["b-m-p-s-p-i"], SENDER))
        rebuilt = cot_marker.build_marker_cot(decoded, OURS, "PEER",
                                              "2026-09-11T20:00:00.000Z",
                                              "2026-09-11T20:00:20.000Z")
        self.assertIn('uid="%s.SPI1"' % OURS, rebuilt)
        self.assertNotIn("ANDROID", rebuilt)

    def test_the_spi_type_is_named_once(self):
        """The caller rate limits on it, and a second copy of the string is how
        the two drift apart."""
        self.assertEqual(cot_marker.SPI_TYPE, MARKER["spi_type"])


class StaleTests(unittest.TestCase):
    """Sixteen bits of seconds is eighteen hours, and ATAK writes a stale a
    year out for a spot marker. Clamping turned a permanent marker into one
    that vanished overnight."""

    def test_a_short_life_keeps_second_precision(self):
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["b-m-p-s-p-i"], SENDER))
        self.assertEqual(decoded["stale_seconds"], 20)

    def test_a_year_long_marker_does_not_become_a_day(self):
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["b-m-p-s-m"], SENDER))
        self.assertGreater(decoded["stale_seconds"], 30 * 86400)

    def test_the_boundary_between_the_two_units(self):
        self.assertEqual(cot_marker._stale_field(cot_marker.MAX_STALE_UNITS),
                         (cot_marker.MAX_STALE_UNITS, False))
        value, minutes = cot_marker._stale_field(cot_marker.MAX_STALE_UNITS + 1)
        self.assertTrue(minutes)
        self.assertEqual(value, (cot_marker.MAX_STALE_UNITS + 60) // 60)

    def test_nothing_wraps(self):
        """Wrapping is the failure that matters: it turns a year into a minute
        rather than into something obviously wrong."""
        value, minutes = cot_marker._stale_field(100 * 365 * 86400)
        self.assertLessEqual(value, cot_marker.MAX_STALE_UNITS)
        self.assertTrue(minutes)


class CodecTests(unittest.TestCase):
    def test_a_marker_round_trips_through_a_rebuild(self):
        """What the far end hands ATAK has to be something this codec would
        recognise again, or a marker relayed twice would decay."""
        for kind in ("a-h-G", "b-m-p-s-m", "b-m-p-c-cp"):
            decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER[kind], SENDER))
            rebuilt = cot_marker.build_marker_cot(decoded, OURS, "PEER",
                                                  "2026-09-11T20:00:00.000Z",
                                                  "2026-09-11T21:00:00.000Z")
            again = cot_marker.decode(cot_marker.marker_from_cot(rebuilt, SENDER))
            for field in ("uid", "type", "callsign", "lat_e7", "lon_e7", "argb"):
                self.assertEqual(again[field], decoded[field], "%s %s" % (kind, field))

    def test_truncated_and_padded_frames_are_refused(self):
        frame = cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER)
        for cut in range(1, len(frame)):
            self.assertIsNone(cot_marker.decode(frame[:cut]), cut)
        self.assertIsNone(cot_marker.decode(frame + b"extra"))

    def test_a_frame_claiming_both_a_uuid_and_a_suffix_is_refused(self):
        frame = bytearray(cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER))
        # The suffix length is the last byte of the header, not an arbitrary
        # offset: aiming at the wrong one corrupts the latitude instead and
        # the frame still decodes, which is how this test first passed nothing.
        frame[cot_marker.HEADER_BYTES - 1] = 4
        self.assertIsNone(cot_marker.decode(bytes(frame)))

    def test_invalid_utf8_is_refused_not_substituted(self):
        frame = bytearray(cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER))
        # Corrupt a byte of the callsign specifically. The trailing bytes of
        # this frame are the colour, so damaging the end tests nothing.
        callsign = b"R.11.095131"
        at = bytes(frame).index(callsign)
        frame[at + 2] = 0xFF
        self.assertIsNone(cot_marker.decode(bytes(frame)))

    def test_markup_in_a_callsign_cannot_escape_the_rebuilt_event(self):
        """A peer chooses its own callsign and this renders it into XML."""
        decoded = cot_marker.decode(cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER))
        decoded["callsign"] = '</contact><detail evil="1">'
        rebuilt = cot_marker.build_marker_cot(decoded, OURS, "PEER",
                                              "2026-09-11T20:00:00.000Z",
                                              "2026-09-11T21:00:00.000Z")
        again = cot_marker.decode(cot_marker.marker_from_cot(rebuilt, SENDER))
        self.assertEqual(again["callsign"], '</contact><detail evil="1">')


class RemarksTests(unittest.TestCase):
    """A long note must not take the marker down with it."""

    def marker_with(self, remarks):
        return ('<event uid="7c9e6679-7425-40de-944b-e07fc1f90ae7" type="a-h-G" '
                'how="h-g-i-g-o" version="2.0" '
                'start="2026-09-12T09:00:00.000Z" stale="2026-09-12T09:05:00.000Z">'
                '<point lat="40.9601" lon="29.1002" hae="48.0" ce="9.0" le="9.0"/>'
                '<detail><contact callsign="HOSTILE.7"/>'
                '<remarks>%s</remarks></detail></event>' % remarks)

    def test_a_long_ascii_note_is_cut_and_the_marker_survives(self):
        frame = cot_marker.marker_from_cot(self.marker_with("A" * 400), 1)
        decoded = cot_marker.decode(frame)
        self.assertIsNotNone(decoded)
        self.assertTrue(decoded["remarks"].startswith("A"))
        self.assertTrue(decoded["remarks"].endswith("\u2026"))

    def test_a_note_cut_inside_a_turkish_character_still_decodes(self):
        """The defect: slicing encoded bytes at a fixed index splits multibyte
        characters, the far end decodes strictly, and the marker vanishes with
        no error anywhere. Two-byte characters at every offset walk the cut
        across a character boundary."""
        for pad in range(0, 8):
            note = ("x" * pad) + ("\u015f" * 200)   # s-cedilla, two bytes each
            frame = cot_marker.marker_from_cot(self.marker_with(note), 1)
            self.assertIsNotNone(frame, "no frame at pad %d" % pad)
            decoded = cot_marker.decode(frame)
            self.assertIsNotNone(decoded, "marker lost at pad %d" % pad)
            # Decoded at all means the bytes were valid UTF-8, which is the
            # whole point; the content is a prefix of what was typed.
            self.assertTrue(note.startswith(decoded["remarks"].rstrip("\u2026")))

    def test_a_short_note_is_untouched(self):
        decoded = cot_marker.decode(
            cot_marker.marker_from_cot(self.marker_with("3 kat, enkaz alt\u0131nda"), 1))
        self.assertEqual(decoded["remarks"], "3 kat, enkaz alt\u0131nda")


class PayloadKindTests(unittest.TestCase):
    def test_the_marker_kind_is_registered_and_distinct(self):
        kinds = [cot_tier2.VERSION, cot_marker.VERSION]
        self.assertEqual(len(kinds), len(set(kinds)))
        self.assertIn(cot_marker.VERSION, tak_payload.KINDS)

    def test_a_marker_frame_is_not_mistaken_for_another_codec(self):
        import position_codec
        import cot_chat
        frame = cot_marker.marker_from_cot(MARKER["a-h-G"], SENDER)
        self.assertIsNone(position_codec.decode(frame))
        self.assertIsNone(cot_chat.decode(frame))
        with self.assertRaises(ValueError):
            cot_tier2.decode(frame)


if __name__ == "__main__":
    unittest.main()
