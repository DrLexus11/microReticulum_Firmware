"""Turning a compact report into CoT without inventing anything.

TAKCapability.md §7 step 4. The whole gateway is a format change, and a format
change is where carefully-preserved uncertainty gets quietly rounded off: the
firmware flags an unknown altitude rather than sending sea level, and all of
that is wasted if this end writes 0.0 into a field ATAK renders as fact.

CoT has its own value for "not known" -- 9999999.0, not zero -- so most of what
follows checks that unknowns survive the crossing.
"""

import importlib.util
import os
import unittest
import xml.etree.ElementTree as ET
from datetime import datetime, timezone


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(name):
    path = os.path.join(ROOT, "tools", name + ".py")
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


codec = load("position_codec")
gateway = load("cot_gateway")


def event_for(fix, stale_seconds=180, received_at=None):
    xml = gateway.build_cot(fix, "urtn-test", "RAD-TEST", stale_seconds,
                            received_at=received_at)
    # Strip the XML declaration the way a client's parser would not need to.
    return ET.fromstring(xml.decode("utf-8").split("?>", 1)[1])


class UnknownsTests(unittest.TestCase):
    def test_an_unknown_altitude_is_cot_unknown_not_sea_level(self):
        # Zero metres is a place. The firmware goes to the trouble of flagging
        # this; writing 0.0 here throws that away at the last step.
        event = event_for(codec.PositionFix(lat_e7=411234567, lon_e7=291234567))
        self.assertEqual(float(event.find("point").get("hae")), 9999999.0)

    def test_a_known_altitude_is_carried(self):
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, alt_known=True, alt_m=847)
        event = event_for(fix)
        self.assertEqual(float(event.find("point").get("hae")), 847.0)

    def test_a_negative_altitude_is_carried(self):
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, alt_known=True, alt_m=-320)
        event = event_for(fix)
        self.assertEqual(float(event.find("point").get("hae")), -320.0)

    def test_an_unreported_accuracy_is_cot_unknown_not_zero(self):
        # ce=0 claims a perfect fix. No receiver on this mesh can make that
        # claim, and ATAK draws it as a point rather than a circle.
        event = event_for(codec.PositionFix(lat_e7=1, lon_e7=1))
        self.assertEqual(float(event.find("point").get("ce")), 9999999.0)

    def test_a_reported_accuracy_is_carried(self):
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, accuracy_m=12)
        event = event_for(fix)
        self.assertEqual(float(event.find("point").get("ce")), 12.0)

    def test_vertical_error_is_never_claimed(self):
        # Nothing on this path measures it.
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, alt_known=True, alt_m=100)
        event = event_for(fix)
        self.assertEqual(float(event.find("point").get("le")), 9999999.0)

    def test_an_absent_track_is_absent_rather_than_zero(self):
        # A track of zero reads as stationary and facing north, which is a
        # claim. No track reads as not reported, which is the truth.
        event = event_for(codec.PositionFix(lat_e7=1, lon_e7=1))
        self.assertIsNone(event.find("detail/track"))

    def test_a_course_of_due_north_is_still_reported(self):
        # Zero degrees is a heading, and it must not be mistaken for absence.
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, course_known=True, course_ddeg=0)
        track = event_for(fix).find("detail/track")
        self.assertIsNotNone(track)
        self.assertEqual(float(track.get("course")), 0.0)


class CoordinateTests(unittest.TestCase):
    def test_coordinates_survive_the_scaling(self):
        fix = codec.PositionFix(lat_e7=411234567, lon_e7=291234567)
        point = event_for(fix).find("point")
        self.assertAlmostEqual(float(point.get("lat")), 41.1234567, places=7)
        self.assertAlmostEqual(float(point.get("lon")), 29.1234567, places=7)

    def test_southern_and_western_coordinates_survive(self):
        fix = codec.PositionFix(lat_e7=-337654321, lon_e7=-583654321)
        point = event_for(fix).find("point")
        self.assertAlmostEqual(float(point.get("lat")), -33.7654321, places=7)
        self.assertAlmostEqual(float(point.get("lon")), -58.3654321, places=7)

    def test_precision_is_not_rounded_away(self):
        # Seven decimal places is what the wire format carries. Printing fewer
        # would discard resolution that survived the whole radio path.
        fix = codec.PositionFix(lat_e7=411234567, lon_e7=291234567)
        self.assertEqual(event_for(fix).find("point").get("lat"), "41.1234567")

    def test_speed_and_course_are_converted_to_cot_units(self):
        # CoT wants metres per second and degrees; the wire carries cm/s and
        # tenths of a degree.
        fix = codec.PositionFix(lat_e7=1, lon_e7=1, speed_cms=500,
                                course_known=True, course_ddeg=1800)
        track = event_for(fix).find("detail/track")
        self.assertAlmostEqual(float(track.get("speed")), 5.0, places=2)
        self.assertAlmostEqual(float(track.get("course")), 180.0, places=1)


class StalenessTests(unittest.TestCase):
    """The field that decides whether a marker is believed."""

    def test_stale_follows_the_reporting_cadence(self):
        received = datetime(2026, 9, 6, 12, 0, 0, tzinfo=timezone.utc)
        event = event_for(codec.PositionFix(lat_e7=1, lon_e7=1),
                          stale_seconds=180, received_at=received)
        time_at = event.get("time")
        stale_at = event.get("stale")
        self.assertEqual(time_at, "2026-09-06T12:00:00.000Z")
        self.assertEqual(stale_at, "2026-09-06T12:03:00.000Z")

    def test_the_default_stale_window_covers_a_missed_report(self):
        # One missed report should not blink a marker off the map; an hour of
        # silence should not leave one on it. Three reporting intervals at the
        # one-a-minute default. Read from the declared default rather than the
        # prose describing it.
        with open(os.path.join(ROOT, "tools", "cot_gateway.py"),
                  encoding="utf-8") as handle:
            source = handle.read()
        self.assertIn('"--stale-seconds", type=int, default=180', source)

    def test_a_clockless_node_is_stamped_with_arrival_not_the_epoch(self):
        # fix_unix_s of zero means the sender had no clock. Trusting it would
        # put the marker in 1970 and ATAK would drop it instantly.
        received = datetime(2026, 9, 6, 12, 0, 0, tzinfo=timezone.utc)
        event = event_for(codec.PositionFix(lat_e7=1, lon_e7=1, fix_unix_s=0),
                          received_at=received)
        self.assertEqual(event.get("start"), "2026-09-06T12:00:00.000Z")

    def test_a_fix_with_its_own_time_keeps_it(self):
        # The time it was taken and the time it arrived are different
        # measurements, and the earlier one is the honest start.
        taken = datetime(2026, 9, 6, 11, 59, 30, tzinfo=timezone.utc)
        received = datetime(2026, 9, 6, 12, 0, 0, tzinfo=timezone.utc)
        fix = codec.PositionFix(lat_e7=1, lon_e7=1,
                                fix_unix_s=int(taken.timestamp()))
        event = event_for(fix, received_at=received)
        self.assertEqual(event.get("start"), "2026-09-06T11:59:30.000Z")
        self.assertEqual(event.get("time"), "2026-09-06T12:00:00.000Z")


class FormatTests(unittest.TestCase):
    def test_the_event_is_wellformed_cot(self):
        xml = gateway.build_cot(codec.PositionFix(lat_e7=1, lon_e7=1),
                                "urtn-test", "RAD-TEST", 180)
        self.assertTrue(xml.startswith(b'<?xml version="1.0" standalone="yes"?>'))
        event = ET.fromstring(xml.decode("utf-8").split("?>", 1)[1])
        self.assertEqual(event.tag, "event")
        self.assertEqual(event.get("version"), "2.0")
        for required in ("uid", "type", "time", "start", "stale", "how"):
            self.assertIsNotNone(event.get(required), "missing %s" % required)
        self.assertIsNotNone(event.find("point"))
        self.assertIsNotNone(event.find("detail/contact"))

    def test_timestamps_are_utc_with_a_z(self):
        event = event_for(codec.PositionFix(lat_e7=1, lon_e7=1))
        for field in ("time", "start", "stale"):
            self.assertTrue(event.get(field).endswith("Z"),
                            "%s is not UTC-marked" % field)

    def test_the_callsign_reaches_the_contact(self):
        xml = gateway.build_cot(codec.PositionFix(lat_e7=1, lon_e7=1),
                                "urtn-abc", "RAD-ABC123", 180)
        event = ET.fromstring(xml.decode("utf-8").split("?>", 1)[1])
        self.assertEqual(event.find("detail/contact").get("callsign"), "RAD-ABC123")

    def test_the_expansion_is_the_point(self):
        # Twenty bytes in, several hundred out. That asymmetry is the entire
        # architecture: compact on the radio, verbose where bandwidth is free.
        fix = codec.PositionFix(lat_e7=411234567, lon_e7=291234567,
                                fix_unix_s=1788681206, accuracy_m=12,
                                alt_known=True, alt_m=847)
        packed = codec.encode(fix)
        expanded = gateway.build_cot(fix, "urtn-test", "RAD-TEST", 180)
        self.assertLessEqual(len(packed), 25)
        self.assertGreater(len(expanded), 10 * len(packed))


if __name__ == "__main__":
    unittest.main()
