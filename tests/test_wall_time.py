"""Architecture checks for the wall-time/monotonic clock separation."""

import os
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
        self.assertGreaterEqual(firmware.count('"/page/time.mu"'), 2)
        self.assertIn("Monotonic ms:", pages)
        self.assertIn("Last source :", pages)
        self.assertIn("BOARD_MODEL == BOARD_OZDISAN_ESP32", display)
        self.assertIn('snprintf(clock_text, sizeof(clock_text), "TIME --")', display)
        self.assertIn('"UP %02llu:%02llu"', display)


if __name__ == "__main__":
    unittest.main()
