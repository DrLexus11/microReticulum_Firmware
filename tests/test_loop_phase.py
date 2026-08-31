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
        # Only the phase enumeration block: everything up to and including
        # LOOP_PHASE_COUNT. Config constants defined after it -- WINDOW_MS,
        # MAGIC -- match the name pattern but are not phases.
        enum_block = self.header[:self.header.index("LOOP_PHASE_COUNT") + 200]
        self.phases = dict(
            (name, int(value))
            for name, value in re.findall(
                r"#define\s+(LOOP_PHASE_\w+)\s+([0-9]+)\s*$", enum_block, re.M)
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

    def test_metric_getters_are_pure(self):
        """The provisioning serializer evaluates a metric lambda more than once.

        A getter that cleared the window returned the value on the first call and
        zero on the second -- and the second is the one serialized. That reported
        an empty window for an interval containing a 2133ms call, which is worse
        than no metric at all. The window must roll on a timer instead.
        """
        self.assertIn("loop_phase_roll_window", self.header)
        self.assertIn("LOOP_PHASE_WINDOW_MS", self.header)
        self.assertNotIn("_and_clear", self.header,
                         "a metric getter must not have side effects")
        # The roll happens on the write path, not a read path.
        roll = self.header[self.header.index("inline void loop_phase(uint8_t phase)"):]
        roll = roll[:roll.index("\n}")]
        self.assertIn("loop_phase_roll_window(now)", roll)

    def test_the_final_phase_is_closed_before_the_watchdog_is_fed(self):
        """Otherwise the last section of loop() is never measured."""
        idx_close = self.sketch.index("loop_phase(LOOP_PHASE_NONE)")
        idx_feed = self.sketch.index("esp_task_wdt_reset()")
        self.assertLess(idx_close, idx_feed)


if __name__ == "__main__":
    unittest.main()
