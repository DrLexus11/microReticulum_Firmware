"""Position on the typed path, which is 94% of everything ATAK emits."""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import cot_position
import cot_tier2
import position_codec

PLI = ('<event uid="ANDROID-7819dadfcf858641" type="a-f-G-U-C" how="m-g" version="2.0">'
       '<point lat="40.9549" lon="29.0934" hae="33.5" ce="44.0" le="9999999.0"/>'
       '<detail><takv device="SAMSUNG SM-A546E" os="36" platform="ATAK-CIV" version="5.6"/>'
       '<contact callsign="LEXUS" endpoint="*:-1:stcp"/>'
       '<__group name="Cyan" role="Team Member"/></detail></event>')

MARKER = ('<event uid="m1" type="a-h-G" how="h-g-i-g-o" version="2.0">'
          '<point lat="40.954" lon="29.094" hae="48.0" ce="9999999.0" le="9999999.0"/>'
          '<detail><contact callsign="R.1"/></detail></event>')


class ClassificationTests(unittest.TestCase):
    def test_a_unit_report_is_position(self):
        self.assertTrue(cot_position.is_position(PLI))

    def test_a_hostile_unit_report_is_also_position(self):
        """The type tail varies with affiliation. All of them are somebody
        saying where a unit is, and all of them belong in the same codec."""
        self.assertTrue(cot_position.is_position(PLI.replace("a-f-G-U-C", "a-h-G-U-C")))

    def test_a_marker_is_not_position(self):
        """A marker is an object on the map, not a unit reporting itself. It
        goes through tier 2, which carries everything the codec has no room for."""
        self.assertFalse(cot_position.is_position(MARKER))

    def test_rubbish_is_not_position(self):
        for bad in ("", "<event", "not xml", "<other/>"):
            self.assertFalse(cot_position.is_position(bad), bad)


class FixExtractionTests(unittest.TestCase):
    def test_a_fix_carries_the_fields_the_wire_has_room_for(self):
        fix = cot_position.fix_from_cot(PLI, sender_id=0x11223344)
        self.assertEqual(fix.lat_e7, 409549000)
        self.assertEqual(fix.lon_e7, 290934000)
        self.assertEqual(fix.accuracy_m, 44)
        self.assertTrue(fix.alt_known)
        self.assertEqual(fix.alt_m, 34)
        self.assertEqual(fix.sender_id, 0x11223344)

    def test_ataks_unknown_sentinel_is_not_taken_as_a_measurement(self):
        """ATAK writes 9999999.0 for "not known". As metres of accuracy that is
        a claim about the whole planet; as altitude it is a track nine thousand
        kilometres up."""
        unknown = PLI.replace('ce="44.0"', 'ce="9999999.0"').replace('hae="33.5"',
                                                                     'hae="9999999.0"')
        fix = cot_position.fix_from_cot(unknown, sender_id=1)
        self.assertEqual(fix.accuracy_m, 0, "0 is the format's word for unreported")
        self.assertFalse(fix.alt_known)

    def test_an_event_without_a_point_carries_no_fix(self):
        self.assertIsNone(cot_position.fix_from_cot(
            '<event uid="a" type="a-f-G-U-C"><detail/></event>', sender_id=1))

    def test_a_malformed_event_is_not_an_exception(self):
        for bad in ("", "<event", "<other/>"):
            self.assertIsNone(cot_position.fix_from_cot(bad, sender_id=1), bad)

    def test_a_fix_round_trips_through_the_wire_format(self):
        """Same nineteen-to-twenty-four bytes the firmware and Columba speak. A
        second dialect of this is how two implementations start disagreeing
        about where somebody is."""
        fix = cot_position.fix_from_cot(PLI, sender_id=0x11223344)
        wire = position_codec.encode(fix)
        self.assertLessEqual(len(wire), position_codec.WIRE_MAX_LEN)
        back = position_codec.decode(wire)
        self.assertEqual((back.lat_e7, back.lon_e7, back.sender_id),
                         (fix.lat_e7, fix.lon_e7, fix.sender_id))


class DiscriminatorTests(unittest.TestCase):
    def test_the_two_codecs_do_not_share_a_first_byte(self):
        """The bridge tells a position report from a tier 2 frame by byte zero.
        That is cheap and it is only safe while the two version numbers differ,
        so the day they collide should be the day this fails rather than the
        day a track lands in the wrong place."""
        self.assertNotEqual(cot_tier2.VERSION, position_codec.WIRE_VERSION)

    def test_a_position_report_does_not_decode_as_tier_2(self):
        fix = cot_position.fix_from_cot(PLI, sender_id=0x11223344)
        with self.assertRaises(ValueError):
            cot_tier2.decode(position_codec.encode(fix))

    def test_a_tier_2_frame_does_not_decode_as_a_position(self):
        self.assertIsNone(position_codec.decode(cot_tier2.encode(MARKER)))


class GateTests(unittest.TestCase):
    """The codec is necessary and not sufficient: ten nodes over four hops is
    85% of a LoRa channel as compressed CoT and 32% as this codec. Sending
    fewer is what makes position affordable."""

    def setUp(self):
        self.gate = cot_position.PositionGate(interval_seconds=60)
        self.fix = cot_position.fix_from_cot(PLI, sender_id=1)

    def test_the_first_report_always_goes(self):
        self.assertTrue(self.gate.allows(self.fix, now=0))

    def test_a_chatty_client_cannot_spend_the_channel(self):
        self.assertTrue(self.gate.allows(self.fix, now=0))
        for second in range(1, 60):
            self.assertFalse(self.gate.allows(self.fix, now=second), second)
        self.assertEqual(self.gate.suppressed, 59)

    def test_the_interval_opens_again(self):
        self.assertTrue(self.gate.allows(self.fix, now=0))
        self.assertTrue(self.gate.allows(self.fix, now=60))

    def test_movement_does_not_wait_out_the_interval(self):
        """A node that has actually moved should not sit behind the floor,
        because that is exactly when its position matters."""
        self.assertTrue(self.gate.allows(self.fix, now=0))
        moved = cot_position.fix_from_cot(
            PLI.replace('lat="40.9549"', 'lat="40.9600"'), sender_id=1)
        self.assertTrue(self.gate.allows(moved, now=1))

    def test_a_step_below_the_threshold_still_waits(self):
        self.assertTrue(self.gate.allows(self.fix, now=0))
        # A metre or so, well under the 25 m threshold.
        nudged = cot_position.fix_from_cot(
            PLI.replace('lat="40.9549"', 'lat="40.95491"'), sender_id=1)
        self.assertFalse(self.gate.allows(nudged, now=1))

    def test_the_threshold_matches_the_firmware(self):
        """PositionReport.h applies the same 25 m, and a gate that disagreed
        would make a RAD and a phone report at different rates for no reason
        anyone could see."""
        source = (Path(__file__).resolve().parents[1] / "PositionReport.h").read_text()
        self.assertIn("#define POSITION_MOVE_THRESHOLD_M %d" % cot_position.MOVE_THRESHOLD_M,
                      source)


if __name__ == "__main__":
    unittest.main()
