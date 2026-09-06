"""The compact position wire format and the send path.

TAKCapability.md §7 step 2. Two implementations of one wire format -- the
firmware encodes, the gateway decodes -- and they are written independently, so
the only thing keeping them honest is a test that reads both. The same reason
test_time_beacon.py cross-checks the signed byte layout.

The rest is the send path, where every mistake available is a mistake about
airtime or about claiming to know something we do not.
"""

import importlib.util
import os
import re
import struct
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def code_only(text):
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def define(header, name):
    match = re.search(r"^#define\s+%s\s+(.+)$" % re.escape(name), header,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    return match.group(1).strip()


def load_codec():
    path = os.path.join(ROOT, "tools", "position_codec.py")
    spec = importlib.util.spec_from_file_location("position_codec", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class WireAgreementTests(unittest.TestCase):
    """If the two ends ever disagree, every report decodes to somewhere else on
    the map -- and it will look like a receiver fault, not a codec fault."""

    def setUp(self):
        self.header = source("PositionReport.h")
        self.codec = load_codec()

    def test_the_version_matches(self):
        self.assertEqual(int(define(self.header, "POSITION_WIRE_VERSION")),
                         self.codec.WIRE_VERSION)

    def test_the_lengths_match(self):
        self.assertEqual(int(define(self.header, "POSITION_WIRE_BASE_LEN")),
                         self.codec.WIRE_BASE_LEN)
        self.assertEqual(int(define(self.header, "POSITION_WIRE_MAX_LEN")),
                         self.codec.WIRE_MAX_LEN)

    def test_the_flag_bits_match(self):
        for name, value in (("ALT", self.codec.FLAG_ALT),
                            ("COURSE", self.codec.FLAG_COURSE),
                            ("SPEED", self.codec.FLAG_SPEED),
                            ("SATS", self.codec.FLAG_SATS)):
            declared = define(self.header, "POSITION_FLAG_%s" % name)
            self.assertEqual(int(declared, 16), value,
                             "POSITION_FLAG_%s disagrees" % name)

    def test_the_base_length_is_what_the_fields_add_up_to(self):
        # version + flags + sender + lat + lon + time + accuracy
        self.assertEqual(self.codec.WIRE_BASE_LEN, 1 + 1 + 4 + 4 + 4 + 4 + 1)

    def test_the_full_report_stays_inside_the_airtime_budget(self):
        # §2: a compact position of about 25 bytes is 44 ms on air and 820
        # reports an hour channel-wide. Raw CoT XML is 538 ms and 67. The whole
        # feature exists because of this difference.
        self.assertLessEqual(self.codec.WIRE_MAX_LEN, 25)


class RoundTripTests(unittest.TestCase):
    def setUp(self):
        self.codec = load_codec()

    def test_a_full_fix_survives_the_round_trip(self):
        fix = self.codec.PositionFix(
            lat_e7=411234567, lon_e7=291234567, fix_unix_s=1788681206,
            accuracy_m=12, alt_known=True, alt_m=847, course_known=True,
            course_ddeg=1800, speed_cms=500, sats=9)
        raw = self.codec.encode(fix)
        self.assertEqual(len(raw), self.codec.WIRE_MAX_LEN)
        back = self.codec.decode(raw)
        self.assertIsNotNone(back)
        self.assertEqual(back.lat_e7, fix.lat_e7)
        self.assertEqual(back.lon_e7, fix.lon_e7)
        self.assertEqual(back.fix_unix_s, fix.fix_unix_s)
        self.assertEqual(back.accuracy_m, fix.accuracy_m)
        self.assertTrue(back.alt_known)
        self.assertEqual(back.alt_m, fix.alt_m)
        self.assertEqual(back.course_ddeg, fix.course_ddeg)
        self.assertEqual(back.speed_cms, fix.speed_cms)
        self.assertEqual(back.sats, fix.sats)

    def test_a_bare_fix_is_the_base_length(self):
        fix = self.codec.PositionFix(lat_e7=-337654321, lon_e7=1512345678)
        raw = self.codec.encode(fix)
        self.assertEqual(len(raw), self.codec.WIRE_BASE_LEN)
        back = self.codec.decode(raw)
        self.assertEqual(back.lat_e7, fix.lat_e7)
        self.assertEqual(back.lon_e7, fix.lon_e7)
        self.assertFalse(back.alt_known)
        self.assertFalse(back.course_known)

    def test_southern_and_western_coordinates_survive(self):
        # Signed int32, not unsigned: half the planet depends on it, and an
        # unsigned bug here puts Sydney in Siberia.
        fix = self.codec.PositionFix(lat_e7=-337654321, lon_e7=-583654321)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertLess(back.lat_e7, 0)
        self.assertLess(back.lon_e7, 0)
        self.assertAlmostEqual(back.lat, -33.7654321, places=6)
        self.assertAlmostEqual(back.lon, -58.3654321, places=6)

    def test_a_negative_altitude_survives(self):
        # Below the ellipsoid is ordinary, and much of the Netherlands is
        # below it by more than a rounding error.
        fix = self.codec.PositionFix(lat_e7=1, lon_e7=1, alt_known=True,
                                     alt_m=-320)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertEqual(back.alt_m, -320)

    def test_accuracy_saturates_rather_than_wraps(self):
        # 400 m of error arriving as 144 is a marker an operator trusts far
        # more than it deserves.
        fix = self.codec.PositionFix(lat_e7=1, lon_e7=1, accuracy_m=400)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertEqual(back.accuracy_m, 255)

    def test_a_full_turn_does_not_wrap_into_a_different_heading(self):
        # A bearing of 360 degrees is due north. Scaled without normalising it
        # becomes 180, which decodes as due south -- a silent 180-degree error
        # in the one field where the person reading the map cannot catch it.
        fix = self.codec.PositionFix(lat_e7=1, lon_e7=1, course_known=True,
                                     course_ddeg=3600)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertEqual(back.course_ddeg, 0)
        # And all three implementations normalise the same way.
        self.assertIn("% 3600", source("PositionReport.h"))

    def test_a_truncated_report_decodes_to_nothing(self):
        # These bytes come off a radio. A short frame is a thing that happens.
        fix = self.codec.PositionFix(lat_e7=1, lon_e7=1, alt_known=True,
                                     alt_m=100)
        raw = self.codec.encode(fix)
        self.assertIsNone(self.codec.decode(raw[:-1]))
        self.assertIsNone(self.codec.decode(b""))
        self.assertIsNone(self.codec.decode(None))

    def test_a_future_version_decodes_to_nothing(self):
        fix = self.codec.PositionFix(lat_e7=1, lon_e7=1)
        raw = bytearray(self.codec.encode(fix))
        raw[0] = 99
        self.assertIsNone(self.codec.decode(bytes(raw)))

    def test_the_firmware_layout_matches_the_python_struct(self):
        # Read the firmware's own encoder order and confirm it writes the
        # fields the Python format string expects, big-endian.
        header = source("PositionReport.h")
        enc = header[header.index("inline size_t position_report_encode("):]
        enc = enc[:enc.index("\n}")]
        # lat then lon then time then accuracy, each MSB first.
        order = [m for m in re.findall(r">> 24|>> 16|>> 8", enc)]
        self.assertEqual(order[:3], [">> 24", ">> 16", ">> 8"])
        self.assertEqual(enc.count(">> 24"), 4)   # sender, lat, lon, secs
        codec = load_codec()
        self.assertEqual(struct.calcsize(">BBIiiIB"), codec.WIRE_BASE_LEN)


class SenderIdentityTests(unittest.TestCase):
    """A report nobody can attribute is a track nobody can follow."""

    def setUp(self):
        self.codec = load_codec()
        self.header = source("PositionReport.h")

    def test_the_sender_survives_the_round_trip(self):
        fix = self.codec.PositionFix(sender_id=0xDEADBEEF, lat_e7=1, lon_e7=1)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertEqual(back.sender_id, 0xDEADBEEF)

    def test_the_high_bit_is_not_lost_to_a_signed_read(self):
        # Four bytes of a hash are as likely as not to have the top bit set,
        # and reading them signed would make two senders collide on one track
        # about half the time.
        fix = self.codec.PositionFix(sender_id=0xFFFFFFFF, lat_e7=1, lon_e7=1)
        back = self.codec.decode(self.codec.encode(fix))
        self.assertEqual(back.sender_id, 0xFFFFFFFF)

    def test_version_one_is_refused_rather_than_read_anonymously(self):
        # A v1 report has no sender, so accepting it would put every report on
        # its own track -- the exact failure v2 exists to fix.
        fix = self.codec.PositionFix(sender_id=1, lat_e7=1, lon_e7=1)
        raw = bytearray(self.codec.encode(fix))
        raw[0] = 1
        self.assertIsNone(self.codec.decode(bytes(raw)))

    def test_the_firmware_stamps_its_own_identity(self):
        # The fix is a measurement and does not know whose it is; the send path
        # does, and takes it there so a relayed report keeps the identity of
        # whoever observed it.
        loop = self.header[self.header.index("inline void position_report_loop("):]
        self.assertIn("RNS::Transport::identity().hash()", loop)
        self.assertIn("outgoing.sender_id", loop)

    def test_the_gateway_tracks_by_sender_not_by_packet(self):
        # The bug this whole version exists for: a packet hash differs every
        # time, so a uid built from one spawns a new marker per report.
        gw = source("tools/cot_gateway.py")
        on_packet = gw[gw.index("def _on_packet"):]
        self.assertIn('uid = "urtn-%08x" % fix.sender_id', on_packet)
        self.assertNotIn("packet.get_hash()", on_packet)

    def test_the_sender_costs_little_enough_to_be_worth_it(self):
        # Still comfortably inside the §2 budget after the addition.
        self.assertLessEqual(self.codec.WIRE_MAX_LEN, 25)
        self.assertEqual(self.codec.WIRE_MAX_LEN - self.codec.WIRE_BASE_LEN, 5)


class SendPathTests(unittest.TestCase):
    def setUp(self):
        self.header = source("PositionReport.h")

    def test_a_report_is_a_packet_not_a_link(self):
        # Link establishment measured about eight kilobytes of transient heap
        # on the OZD fixture against roughly twenty-three free -- the deepest
        # single allocation on that board. Paying it every minute to deliver
        # twenty bytes would be the most expensive thing the node does, and a
        # position report needs neither a session nor a reply.
        code = code_only(self.header)
        self.assertIn("RNS::Packet packet(gateway", code)
        self.assertNotIn("RNS::Link", code)

    def test_it_unicasts_to_a_fixed_gateway_rather_than_announcing(self):
        # §3. Reticulum is not a flooding network: announcing every minute to
        # carry twenty bytes spends more on routing than on the payload.
        code = code_only(self.header)
        self.assertIn("Destination::OUT", code)
        self.assertIn("Destination::SINGLE", code)
        self.assertNotIn(".announce(", code)

    def test_an_unconfigured_node_reports_nothing(self):
        # A node not told where to send its position must not guess.
        loop = self.header[self.header.index("inline void position_report_loop("):]
        self.assertIn("if (position_gateway_hash.size() == 0) return;",
                      loop[:400])

    def test_a_stale_fix_is_never_sent(self):
        # A marker on a map is a claim about now. Repeating a lapsed fix is how
        # a search team ends up somewhere the node used to be.
        loop = self.header[self.header.index("inline void position_report_loop("):]
        self.assertIn("node_position_is_stale(fix)", loop)
        self.assertIn("skipped_no_fix++", loop)

    def test_a_stationary_node_still_reports(self):
        # Silence is not "unchanged" to a TAK client -- it is a track going
        # stale and vanishing off the map.
        self.assertIn("POSITION_REPORT_INTERVAL_MS", self.header)
        interval = define(self.header, "POSITION_REPORT_INTERVAL_MS")
        self.assertGreaterEqual(int(interval.rstrip("ULul")), 30000)

    def test_movement_can_beat_the_heartbeat(self):
        # A node that has moved should not wait out the interval to say so;
        # that is precisely when its position matters.
        code = code_only(self.header)
        self.assertIn("position_has_moved", code)
        loop = code[code.index("inline void position_report_loop("):]
        self.assertIn("position_has_moved(fix, st)", loop)

    def test_the_movement_gate_errs_toward_reporting(self):
        # The latitude scale is applied to longitude too. A degree of longitude
        # shortens with latitude, so real movement produces a larger delta than
        # this assumes and the gate fires early. Missing movement that happened
        # is the failure that matters; reporting movement that did not is
        # merely airtime.
        self.assertIn("POSITION_MOVE_THRESHOLD_E7", self.header)
        self.assertIn("111320", self.header)

    def test_the_cadence_is_jittered(self):
        # A fleet powered up together would otherwise report in lockstep
        # forever, which is the worst possible arrangement of the same airtime.
        code = code_only(self.header)
        self.assertIn("POSITION_REPORT_JITTER_MS", code)
        self.assertIn("random(POSITION_REPORT_JITTER_MS)", code)

    def test_a_missing_path_is_reported_once_not_every_cadence(self):
        # A standing condition is not an event. TimeSync learned this on the
        # board that could least afford the console traffic.
        self.assertIn("reported_waiting", self.header)
        loop = self.header[self.header.index("inline void position_report_loop("):]
        self.assertIn("if (!st.reported_waiting)", loop)

    def test_it_asks_for_a_path_rather_than_giving_up(self):
        loop = self.header[self.header.index("inline void position_report_loop("):]
        self.assertIn("Transport::request_path(position_gateway_hash)", loop)

    def test_counters_separate_the_reasons_for_silence(self):
        # No fix, no path and an encode failure are three different faults with
        # three different fixes, and they all look like "no reports arriving".
        for counter in ("sent", "skipped_no_fix", "skipped_no_path",
                        "path_requests", "failures"):
            self.assertIn("uint32_t %s = 0" % counter, self.header)


class ProvisioningTests(unittest.TestCase):
    def test_the_gateway_is_provisionable(self):
        self.assertIn("PROV_GENERAL_POSITION_GATEWAY", source("Provisioning.h"))
        cpp = source("Provisioning.cpp")
        self.assertIn("position_gateway_hash = v.as_bytes()", cpp)

    def test_the_compact_profile_does_not_carry_it(self):
        # The compact schema exists because roughly a hundred retained fields
        # exhausted the OZD's heap before RNS could start. That board is the
        # radio-less reverse-path fixture and has no position to report.
        cpp = source("Provisioning.cpp")
        self.assertEqual(cpp.count("PROV_GENERAL_POSITION_GATEWAY"), 1)


class WiringTests(unittest.TestCase):
    def test_the_loop_runs(self):
        sketch = source("RNode_Firmware.ino")
        self.assertIn('#include "PositionReport.h"', sketch)
        self.assertIn("position_report_loop();", sketch)

    def test_it_compiles_out_without_reticulum(self):
        header = source("PositionReport.h")
        self.assertIn("inline void position_report_loop() {}", header)


if __name__ == "__main__":
    unittest.main()
