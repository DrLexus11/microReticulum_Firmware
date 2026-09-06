"""The position source seam.

TAKCapability.md §7 step 1. A position that is wrong on a map is worse than no
position, because it is believed and acted on -- so most of what matters here
is about refusing to state more than is known: an unknown altitude that is not
sea level, an unreported accuracy that is not zero error, a fix whose clock we
cannot vouch for that is not stamped with ours.

The rest is about a board that has run out of heap before. This runs in the
main loop on the OZD fixture, which aborted on a 32-byte allocation this
morning.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def code_only(text):
    """Strip // comments. These headers explain themselves at length, and the
    prose names the very constructs the tests forbid."""
    return "\n".join(line.split("//")[0] for line in text.splitlines())


def define(header, name):
    match = re.search(r"^#define\s+%s\s+(.+)$" % re.escape(name), header,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    return match.group(1).strip()


class AllocationTests(unittest.TestCase):
    def test_the_seam_allocates_nothing(self):
        # Polled from the main loop on a fixture that lives at ten percent free
        # heap. std::function allocates on construction, and a vector of
        # sources would grow at exactly the wrong moment.
        code = code_only(source("Position.h"))
        for banned in ("std::function", "std::vector", "std::string",
                       "std::map", "new ", "malloc("):
            self.assertNotIn(banned, code)

    def test_sources_live_in_a_fixed_array(self):
        header = source("Position.h")
        self.assertIn("NODE_POSITION_MAX_SOURCES", header)
        self.assertIn("node_position_sources[NODE_POSITION_MAX_SOURCES]",
                      header)

    def test_a_source_poll_is_a_plain_function_pointer(self):
        code = code_only(source("Position.h"))
        self.assertIn("using NodePositionPoll = bool (*)(NodePositionFix&", code)

    def test_registering_past_capacity_says_so_rather_than_overflowing(self):
        # Silently dropping a source produces a node that reports no position
        # and no reason, which is the hardest kind of fault to find.
        header = source("Position.h")
        body = header[header.index("inline bool node_position_register("):]
        body = body[:body.index("\n}")]
        self.assertIn(">= NODE_POSITION_MAX_SOURCES", body)
        self.assertIn("printf(", body)
        self.assertIn("return false", body)


class HonestyTests(unittest.TestCase):
    """Zero is a real value for most of these fields."""

    def test_an_unknown_altitude_is_flagged_not_zeroed(self):
        # Zero metres is sea level, which is a place. A receiver with no
        # altitude solution must not be read as reporting one.
        header = source("Position.h")
        self.assertIn("bool alt_known", header)

    def test_an_unknown_course_is_flagged_not_zeroed(self):
        # Zero degrees is due north.
        header = source("Position.h")
        self.assertIn("bool course_known", header)

    def test_a_fix_without_a_clock_is_not_stamped_with_ours(self):
        # The time we received a fix and the time it was taken are different
        # measurements. Substituting one for the other is the same error as
        # advertising uptime as a timebase, which this firmware has made before.
        header = source("Position.h")
        self.assertIn("uint64_t fix_unix_ms = 0", header)
        loop = header[header.index("inline void node_position_loop()"):]
        # received_ms may be defaulted from millis(); fix_unix_ms may not.
        self.assertIn("fix.received_ms = now", loop)
        self.assertNotIn("fix.fix_unix_ms = now", loop)
        self.assertNotIn("fix.fix_unix_ms = OS::wall_time", loop)

    def test_there_is_no_sentinel_coordinate(self):
        # Null island is a real place people have been sent. Validity is a flag.
        header = source("Position.h")
        self.assertIn("bool valid = false", header)
        self.assertNotIn("LAT_INVALID", header)
        self.assertNotIn("999", header)

    def test_coordinates_are_scaled_integers_not_floats(self):
        # This struct becomes a wire format in step 2, and a float on the wire
        # is a portability question nobody needs to answer.
        header = source("Position.h")
        self.assertIn("int32_t lat_e7", header)
        self.assertIn("int32_t lon_e7", header)
        code = code_only(header)
        for floaty in ("float lat", "double lat", "float lon", "double lon"):
            self.assertNotIn(floaty, code)


class StalenessTests(unittest.TestCase):
    """What ATAK actually drops a track for."""

    def test_age_is_measured_on_the_monotonic_clock(self):
        # A node with no wall time still must not report an old position as
        # current, and most of this fleet has no wall time until an authority
        # reaches it.
        header = source("Position.h")
        age = header[header.index("inline uint32_t node_position_age_ms("):]
        age = age[:age.index("\n}")]
        self.assertIn("millis()", age)
        self.assertNotIn("wall_time", age)

    def test_age_is_wrap_corrected(self):
        # millis() wraps at 49.7 days, which is well inside the uptime we are
        # now asking these boards to reach.
        header = source("Position.h")
        age = header[header.index("inline uint32_t node_position_age_ms("):]
        age = age[:age.index("\n}")]
        self.assertIn("(uint32_t)(millis() - fix.received_ms)", age)

    def test_a_lapsed_fix_is_cleared_rather_than_held(self):
        # Holding the last known position forever turns a receiver that stopped
        # answering into a node confidently reporting somewhere it no longer is.
        header = source("Position.h")
        loop = header[header.index("inline void node_position_loop()"):]
        self.assertIn("node_position_current = NodePositionFix{}", loop)

    def test_the_stale_window_is_measured_in_minutes(self):
        # §2 fixes the position cadence in minutes, not seconds. A window
        # shorter than a couple of missed reports would flap.
        header = source("Position.h")
        window = int(define(header, "NODE_POSITION_STALE_MS").rstrip("ULul"))
        self.assertGreaterEqual(window, 60000)


class SelectionTests(unittest.TestCase):
    def test_selection_is_deterministic_rank_then_freshness(self):
        # An operator moving a node between sources should be able to predict
        # what it reports, rather than discover an emergent quality metric.
        header = source("Position.h")
        loop = header[header.index("inline void node_position_loop()"):]
        self.assertIn("s.rank < chosen->rank", loop)
        self.assertIn("node_position_age_ms(fix) < node_position_age_ms(best)",
                      loop)

    def test_a_distrusted_clock_costs_the_timestamp_not_the_position(self):
        # Where the node is and when it was there are separable. Dropping the
        # position because its clock is wrong loses information we have.
        header = source("Position.h")
        loop = header[header.index("inline void node_position_loop()"):]
        skew = loop[loop.index("rejected_skew++"):]
        self.assertIn("fix.fix_unix_ms = 0", skew[:400])

    def test_a_silent_source_is_distinguishable_from_an_absent_one(self):
        # The same lesson as [timesync] reporting why it is waiting: a source
        # registered but never producing looks exactly like one never
        # registered, and that difference is the whole diagnosis.
        header = source("Position.h")
        for counter in ("fixes", "rejected_stale", "rejected_skew"):
            self.assertIn("uint32_t %s = 0" % counter, header)


class WiringTests(unittest.TestCase):
    def test_the_loop_runs(self):
        sketch = source("RNode_Firmware.ino")
        self.assertIn('#include "Position.h"', sketch)
        self.assertIn("node_position_loop();", sketch)

    def test_it_compiles_out_without_reticulum(self):
        # Position has no meaning without a mesh to report it to, and the
        # radio-less variants must not pay for it.
        header = source("Position.h")
        self.assertIn("#if defined(HAS_RNS)", header)
        self.assertIn("inline void node_position_loop() {}", header)


if __name__ == "__main__":
    unittest.main()
