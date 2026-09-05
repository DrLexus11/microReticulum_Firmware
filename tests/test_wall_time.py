"""Architecture checks for the wall-time/monotonic clock separation."""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


class WallTimeArchitectureTests(unittest.TestCase):
    def test_ntp_is_station_only_and_never_changes_the_logical_clock(self):
        clock = source("WallTime.h")
        self.assertIn("wifi_mode == WR_WIFI_STA", clock)
        self.assertIn("!wifi_ap_fallback_active", clock)
        self.assertIn("configTime(0, 0", clock)
        self.assertIn("Reticulum::adopt_wall_time", clock)
        self.assertNotIn("setTimeOffset", clock)

    def test_ntp_does_not_write_flash_on_every_loop(self):
        clock = source("WallTime.h")
        self.assertIn("WALL_TIME_NTP_RESYNC_POLL_MS 21600000UL", clock)
        self.assertIn("WALL_TIME_NTP_MIN_CORRECTION_MS 1000ULL", clock)

    def test_unknown_time_stays_explicit_on_wire(self):
        propagation = source("LXMFPropagation.h")
        peer = source("LXMFPeerSync.h")
        bridge = source("RRCBridge.cpp")
        self.assertIn("OS::wall_time_known()", propagation)
        self.assertIn("OS::wall_time_known()", peer)
        self.assertIn("OS::wall_time_known()", bridge)
        self.assertIn(": 0ULL", propagation)
        self.assertIn(": 0ULL", peer)
        self.assertIn(": 0.0", bridge)

    def test_validation_surfaces_are_registered(self):
        firmware = source("RNode_Firmware.ino")
        pages = source("Pages.h")
        display = source("Display.h")
        # Read-only and public: a peer must be able to ask whether our clock is
        # trustworthy before it believes anything we timestamped. Registering it
        # once, outside the NOMADNET_PAGES_ALLOW_ALL split, is what keeps it
        # reachable on production builds instead of silently timing out.
        self.assertEqual(firmware.count('"/page/time.mu"'), 1)
        self.assertIn(
            'register_request_handler("/page/time.mu", serve_page, '
            'RNS::Type::Destination::ALLOW_ALL)', firmware)
        self.assertNotIn(
            'register_request_handler("/page/time.mu", serve_page, '
            'RNS::Type::Destination::ALLOW_LIST', firmware)
        self.assertIn("Monotonic ms:", pages)
        self.assertIn("Last source :", pages)
        self.assertIn("BOARD_MODEL == BOARD_OZDISAN_ESP32", display)
        self.assertIn('snprintf(clock_text, sizeof(clock_text), "TIME --")', display)
        self.assertIn('"UP %02llu:%02llu"', display)

    def test_every_environment_pins_the_same_library_revision(self):
        # The wall-time API lives in microReticulum, and Pages.h, LXMF* and
        # RRCBridge.cpp now call it unconditionally. Bumping only the
        # environments under test leaves every other board pinned to a library
        # without those symbols, where the failure is a wall of "is not a member
        # of RNS::Utilities::OS" rather than anything that names the real cause.
        pins = re.findall(r"microReticulum\.git#([0-9a-fA-F]+)",
                          source("platformio.ini"))
        self.assertTrue(pins, "no pinned microReticulum revisions found")
        self.assertEqual(sorted(set(pins)), sorted({pins[0]}),
                         "platformio.ini pins more than one microReticulum "
                         "revision: %s" % sorted(set(pins)))


if __name__ == "__main__":
    unittest.main()
