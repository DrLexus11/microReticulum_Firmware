"""Tier 2: carrying any CoT event, compressed by a shipped dictionary."""

import sys
import unittest
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_tier2 as tier2

# Real ATAK-CIV 5.6.0.12 events, captured 2026-09-10.
MARKER = ('<event access="Undefined" how="h-g-i-g-o" stale="2026-09-10T11:47:07.866Z" '
          'start="2026-09-10T11:42:07.866Z" time="2026-09-10T11:42:07.866Z" type="a-h-G" '
          'uid="66d6de40-bd62-4a5e-92ef-d4ee14b0194a" version="2.0">'
          '<point ce="9999999.0" hae="48.019" lat="40.9545566" le="9999999.0" lon="29.0945023"/>'
          '<detail><status readiness="true"/><archive/><color argb="-1"/>'
          '<contact callsign="R.10.144053"/>'
          '<usericon iconsetpath="COT_MAPPING_2525C/a-h/a-h-G"/>'
          '<creator callsign="LEXUS" time="2026-09-10T11:40:53.380Z" type="a-f-G-U-C" '
          'uid="ANDROID-7819dadfcf858641"/><remarks/></detail></event>')

PLI = ('<event access="Undefined" how="m-g" stale="2026-09-10T11:42:51.243Z" '
       'start="2026-09-10T11:42:03.133Z" time="2026-09-10T11:42:03.133Z" type="a-f-G-U-C" '
       'uid="ANDROID-7819dadfcf858641" version="2.0">'
       '<point ce="44.0" hae="33.593" lat="40.954935" le="9999999.0" lon="29.093375"/>'
       '<detail><takv device="SAMSUNG SM-A546E" os="36" platform="ATAK-CIV" version="5.6.0.12"/>'
       '<contact callsign="LEXUS" endpoint="*:-1:stcp"/><uid Droid="LEXUS"/>'
       '<precisionlocation altsrc="GPS" geopointsrc="GPS"/>'
       '<__group abbr="SV" exrole="Surveillance" name="Cyan" role="Team Member"/>'
       '<status battery="59"/><track course="8.27" speed="2.87"/></detail></event>')


class RoundTripTests(unittest.TestCase):
    def test_real_events_round_trip_exactly(self):
        for name, xml in (("marker", MARKER), ("pli", PLI)):
            self.assertEqual(tier2.decode(tier2.encode(xml)), xml, name)

    def test_non_ascii_survives(self):
        xml = '<event type="a-f-G" uid="x"><detail><remarks>Kılavuz café</remarks></detail></event>'
        self.assertEqual(tier2.decode(tier2.encode(xml)), xml)

    def test_bytes_and_text_are_equivalent(self):
        self.assertEqual(tier2.encode(MARKER), tier2.encode(MARKER.encode("utf-8")))


class SizeTests(unittest.TestCase):
    def test_real_events_fit_one_reticulum_packet(self):
        """The point of the dictionary, and the reason 2.5x is worth having.

        A Reticulum packet carries 383 bytes encrypted. Raw, both of these need
        two; compressed, they need one, which halves their airtime.
        """
        for name, xml in (("marker", MARKER), ("pli", PLI)):
            frame = tier2.encode(xml)
            self.assertGreater(len(xml), 383, name)
            self.assertLessEqual(len(frame), 383, "%s: %d bytes" % (name, len(frame)))

    def test_a_frame_never_costs_more_air_than_the_event_itself(self):
        """Deflate can expand incompressible input, and paying airtime for
        compression that made the packet bigger hides until somebody measures.

        Asserted as the property rather than the mechanism: which encoding wins
        is an implementation detail, never exceeding raw+header is not. The
        dictionary is good enough that even a 25-byte event still compresses,
        so a case that genuinely does not is built from random bytes.
        """
        import os
        incompressible = '<event uid="%s"/>' % os.urandom(400).hex()
        for xml in (MARKER, PLI, '<event type="a" uid="b"/>', incompressible):
            frame = tier2.encode(xml)
            self.assertLessEqual(len(frame), len(xml.encode("utf-8")) + 2, xml[:40])
            self.assertEqual(tier2.decode(frame), xml, xml[:40])

    def test_the_raw_encoding_still_decodes(self):
        """The uncompressed path is the safety valve, so it must work even
        when nothing currently chooses it."""
        frame = bytes([tier2.VERSION, tier2.ENCODING_RAW]) + MARKER.encode("utf-8")
        self.assertEqual(tier2.decode(frame), MARKER)


class RejectionTests(unittest.TestCase):
    def test_unknown_version_and_encoding_are_refused(self):
        good = tier2.encode(MARKER)
        for frame in (bytes([99, tier2.ENCODING_RAW]) + b"<event/>",
                      bytes([tier2.VERSION, 200]) + b"<event/>"):
            with self.assertRaises(ValueError):
                tier2.decode(frame)
        self.assertEqual(tier2.decode(good), MARKER)

    def test_short_and_empty_frames_are_refused(self):
        for frame in (b"", b"\x01", bytes([tier2.VERSION, tier2.ENCODING_RAW]), None, "text"):
            with self.assertRaises(ValueError):
                tier2.decode(frame)

    def test_corrupt_compressed_payload_is_a_value_error_not_a_crash(self):
        frame = bytearray(tier2.encode(MARKER))
        frame[-1] ^= 0xFF
        frame[5] ^= 0xFF
        with self.assertRaises(ValueError):
            tier2.decode(bytes(frame))

    def test_a_decompression_bomb_is_refused(self):
        """A peer can send a few hundred bytes that expand to megabytes.

        CoT that large is not tier 2 traffic at all -- it belongs in tier 3,
        fetched deliberately with the cost shown -- so refusing is correct.
        """
        compressor = zlib.compressobj(9, zlib.DEFLATED, -15, zdict=tier2.DICTIONARY)
        bomb = compressor.compress(b"<event/>" * 200000) + compressor.flush()
        frame = bytes([tier2.VERSION, tier2.ENCODING_DEFLATE_DICT_V1]) + bomb
        self.assertLess(len(frame), 4096, "the bomb must be small to be a bomb")
        with self.assertRaises(ValueError):
            tier2.decode(frame)

    def test_oversized_input_is_refused_at_encode(self):
        with self.assertRaises(ValueError):
            tier2.encode("<event/>" * 100000)


class DictionaryTests(unittest.TestCase):
    def test_the_shipped_dictionary_contains_no_captured_traffic(self):
        """It ships to every node, so it must not carry anyone's callsign,
        coordinates or device id. A dictionary trained on real events
        compresses better and is refused for exactly this reason."""
        blob = tier2.DICTIONARY.decode("utf-8")
        for leaked in ("LEXUS", "ANDROID-7819", "R.10.144053", "40.954", "29.09",
                       "SM-A546E", "66d6de40"):
            self.assertNotIn(leaked, blob, leaked)


if __name__ == "__main__":
    unittest.main()
