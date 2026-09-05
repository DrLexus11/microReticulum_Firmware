"""Architecture checks for periodic signed time assertions.

Step 3 removed the human from the loop but still needs a link to a specific
peer. A node that can only listen -- OZD-01 on the bench, unreachable, its OLED
showing an uptime counter where a clock should be -- can never use it. These
guard the properties that make the broadcast path safe rather than merely
working, because every one of them fails silently if it regresses.
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


def load_authority():
    path = os.path.join(ROOT, "tools", "time_authority.py")
    spec = importlib.util.spec_from_file_location("time_authority", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def define(header, name):
    match = re.search(r"^#define\s+%s\s+(.+)$" % re.escape(name), header,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    return match.group(1).strip()


class SignedBytesTests(unittest.TestCase):
    """The two ends build the message independently. If they ever disagree,
    every signature fails to verify and the failure is indistinguishable from
    an attack -- so the agreement is asserted, not assumed."""

    def test_domain_separator_matches_the_firmware(self):
        header = source("TimeBeacon.h")
        domain = define(header, "TIME_BEACON_DOMAIN").strip('"')
        try:
            authority = load_authority()
        except ImportError:
            self.skipTest("RNS is not importable in this environment")
        self.assertEqual(authority.DOMAIN.decode(), domain)
        self.assertEqual(int(define(header, "TIME_BEACON_DOMAIN_LEN")),
                         len(domain))

    def test_signed_message_length_matches_the_firmware(self):
        header = source("TimeBeacon.h")
        domain_len = int(define(header, "TIME_BEACON_DOMAIN_LEN"))
        # unix_ms u64, valid_for u32, stratum u8, source u8
        self.assertIn("(TIME_BEACON_DOMAIN_LEN + 8 + 4 + 1 + 1)",
                      define(header, "TIME_BEACON_SIGNED_LEN"))
        try:
            authority = load_authority()
        except ImportError:
            self.skipTest("RNS is not importable in this environment")
        built = authority.signed_bytes(1788592757819, 7200, 1, 2)
        self.assertEqual(len(built), domain_len + 8 + 4 + 1 + 1)
        self.assertEqual(built[:domain_len], authority.DOMAIN)
        self.assertEqual(struct.unpack(">Q", built[domain_len:domain_len + 8])[0],
                         1788592757819)
        self.assertEqual(built[-2], 1)   # stratum
        self.assertEqual(built[-1], 2)   # source: NTP

    def test_domain_differs_from_the_solicited_reply(self):
        # The solicited path signs nonce||unix_ms||stratum with no domain at
        # all. Without a distinct prefix here, a broadcast assertion and a
        # nonce-challenged one could be substituted for each other.
        header = source("TimeBeacon.h")
        domain = define(header, "TIME_BEACON_DOMAIN").strip('"')
        self.assertTrue(domain)
        self.assertIn("uint8_t message[17]", source("TimeSync.h"))
        self.assertNotIn(domain, source("TimeSync.h"))


class TrustTests(unittest.TestCase):
    def test_an_empty_authority_list_refuses_the_broadcast_path(self):
        # Everywhere else in this firmware an empty list means "trust any IFAC
        # peer", which is the membership model. Originating time is the
        # documented exception: a beacon needs no reply and reaches the whole
        # mesh, so with nothing provisioned we adopt nothing.
        header = source("TimeBeacon.h")
        apply_body = header[header.index("inline void time_beacon_apply("):
                            header.index("class TimeBeaconAnnounceHandler")]
        empty = apply_body.index("if (time_sync_authorities.empty())")
        adopt = apply_body.index("adopt_wall_time")
        self.assertLess(empty, adopt)
        self.assertIn("st.refused_unlisted++", apply_body[empty:adopt])

    def test_the_rollback_floor_only_moves_after_verification(self):
        # A forged assertion that raised the floor would lock out the real
        # authority permanently -- a denial of service that survives reboots
        # only if it is written, so keep it in RAM and set it late.
        header = source("TimeBeacon.h")
        apply_body = header[header.index("inline void time_beacon_apply("):
                            header.index("class TimeBeaconAnnounceHandler")]
        validate = apply_body.index("announced_identity.validate(")
        floor = apply_body.index("st.highest_asserted_ms = unix_ms")
        self.assertLess(validate, floor)

    def test_a_replayed_assertion_cannot_move_the_clock(self):
        header = source("TimeBeacon.h")
        self.assertIn("if (unix_ms <= st.highest_asserted_ms)", header)

    def test_a_node_without_a_credible_clock_does_not_originate(self):
        # `persisted` is a lower bound, not a measurement. Beaconing it would
        # spread a plausible wrong answer to nodes that cannot check it.
        header = source("TimeBeacon.h")
        loop = header[header.index("inline void time_beacon_loop("):]
        self.assertIn("!OS::wall_time_known()", loop)
        self.assertIn("WallTimeSource::PERSISTED", loop)
        self.assertIn("OS::wall_time_stratum() == 0", loop)
        self.assertLess(loop.index("wall_time_known"), loop.index("announce("))

    def test_provenance_is_recorded_as_a_beacon_not_as_a_reference(self):
        # A listen-only node cannot prove freshness, so it must never claim the
        # authority's own provenance.
        header = source("TimeBeacon.h")
        self.assertIn("OS::WallTimeSource::SIGNED_BEACON", header)
        self.assertNotIn("WallTimeSource::NTP", header)

    def test_our_own_beacon_is_ignored(self):
        # It comes back over any interface that loops.
        header = source("TimeBeacon.h")
        handler = header[header.index("class TimeBeaconAnnounceHandler"):
                         header.index("inline RNS::Bytes time_beacon_assertion(")]
        self.assertIn("destination_hash == time_beacon_destination.hash()", handler)


class ObservabilityTests(unittest.TestCase):
    def test_the_clock_page_reports_distribution_not_only_the_value(self):
        # A node hearing assertions and refusing them looks exactly like one
        # hearing nothing, and the difference is the whole diagnosis.
        pages = source("Pages.h")
        for field in ("Authorities", "Heard", "Verified", "Adopted",
                      "not an authority", "bad signature", "stale"):
            self.assertIn(field, pages)

    def test_a_stalled_time_client_says_so(self):
        # Both of these paths returned silently, which made a client that never
        # got anywhere indistinguishable from one that was never configured.
        sync = source("TimeSync.h")
        self.assertIn("no path to <", sync)
        self.assertIn("but no identity for it yet", sync)
        # Once, not every poll: a standing condition is not an event.
        self.assertIn("st.reported_waiting", sync)


if __name__ == "__main__":
    unittest.main()
