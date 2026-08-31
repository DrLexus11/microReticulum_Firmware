"""The loop-phase breadcrumb used to diagnose TASK_WDT resets.

CarriedIssues.md §1: the node resets with TASK_WDT at irregular intervals and the
watchdog names only the task, which is always loopTask. LoopPhase.h records which
call inside loop() was running, in RTC memory that survives the reset.

A phase without a name, or a marker that never made it into loop(), makes the
diagnostic silently useless -- it reports a number nobody can interpret, or
reports the wrong phase because the marker is missing.
"""

import os
import re
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HEADER = os.path.join(ROOT, "LoopPhase.h")
SKETCH = os.path.join(ROOT, "RNode_Firmware.ino")


class LoopPhaseTests(unittest.TestCase):
    def setUp(self):
        with open(HEADER, "r", encoding="utf-8") as handle:
            self.header = handle.read()
        with open(SKETCH, "r", encoding="utf-8") as handle:
            self.sketch = handle.read()
        self.phases = dict(
            (name, int(value))
            # Decimal only and anchored to end of line: LOOP_PHASE_MAGIC is a hex
            # literal, and a bare digit match picked up the "0" of its "0x...",
            # inventing a phase that then failed every check.
            for name, value in re.findall(
                r"#define\s+(LOOP_PHASE_\w+)\s+([0-9]+)\s*$", self.header, re.M)
        )

    def test_every_phase_has_a_name(self):
        """A phase id with no name reports an integer nobody can act on."""
        for name, value in self.phases.items():
            if name in ("LOOP_PHASE_COUNT", "LOOP_PHASE_NONE"):
                continue
            with self.subTest(phase=name):
                self.assertIn("case %s:" % name, self.header,
                              "%s has no entry in loop_phase_name()" % name)

    def test_count_covers_every_phase(self):
        count = self.phases["LOOP_PHASE_COUNT"]
        for name, value in self.phases.items():
            if name == "LOOP_PHASE_COUNT":
                continue
            with self.subTest(phase=name):
                self.assertLess(value, count,
                                "%s is outside LOOP_PHASE_COUNT, so its worst-case "
                                "duration is never recorded" % name)

    def test_every_phase_is_actually_marked_in_the_loop(self):
        """A defined phase that is never entered can never be reported."""
        for name in self.phases:
            if name in ("LOOP_PHASE_COUNT", "LOOP_PHASE_NONE"):
                continue
            with self.subTest(phase=name):
                self.assertIn("loop_phase(%s)" % name, self.sketch,
                              "%s is defined but never entered in loop()" % name)

    def test_the_breadcrumb_survives_a_reset(self):
        """NOINIT is the whole mechanism: a zeroed variable says nothing."""
        self.assertIn("RTC_NOINIT_ATTR uint8_t  loop_phase_current", self.header)
        self.assertIn("LOOP_PHASE_MAGIC", self.header,
                      "a magic must guard against reading uninitialised RTC memory")

    def test_the_final_phase_is_closed_before_the_watchdog_is_fed(self):
        """Otherwise the last section of loop() is never measured."""
        idx_close = self.sketch.index("loop_phase(LOOP_PHASE_NONE)")
        idx_feed = self.sketch.index("esp_task_wdt_reset()")
        self.assertLess(idx_close, idx_feed)


if __name__ == "__main__":
    unittest.main()
