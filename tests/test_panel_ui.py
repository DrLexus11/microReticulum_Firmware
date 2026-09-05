"""Architecture checks for the 128x64 panel.

A layout that overflows does not fail loudly: text runs off the right edge or a
row lands past the last page of the framebuffer and simply is not drawn. The
design's own annotation had this problem before a line of it was written --
`[LR][BT][WF][EN]` plus the clock is 23 characters on a 21-character line -- so
the arithmetic is asserted rather than eyeballed.
"""

import os
import re
import unittest


ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The built-in GFX glyph cell, which DisplayUI.h draws with.
PANEL_WIDTH = 128
PANEL_HEIGHT = 64
CELL_WIDTH = 6
CELL_HEIGHT = 8
COLUMNS = PANEL_WIDTH // CELL_WIDTH   # 21


def source(name):
    with open(os.path.join(ROOT, name), encoding="utf-8") as handle:
        return handle.read()


def define(text, name):
    match = re.search(r"^#define\s+%s\s+(-?\d+)\s*$" % re.escape(name), text,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    return int(match.group(1))


class PanelLayoutTests(unittest.TestCase):
    def setUp(self):
        self.ui = source("DisplayUI.h")

    def test_every_row_fits_the_panel(self):
        rows = ["UI_Y_TITLE", "UI_Y_IFACE", "UI_Y_BODY0", "UI_Y_BODY1",
                "UI_Y_BODY2", "UI_Y_BODY3", "UI_Y_FOOT"]
        for row in rows:
            top = define(self.ui, row)
            self.assertGreaterEqual(top, 0, "%s starts above the panel" % row)
            self.assertLessEqual(
                top + CELL_HEIGHT, PANEL_HEIGHT,
                "%s at y=%d overflows the %dpx panel" % (row, top, PANEL_HEIGHT))

    def test_rows_do_not_overlap(self):
        rows = [define(self.ui, name) for name in
                ["UI_Y_TITLE", "UI_Y_IFACE", "UI_Y_BODY0", "UI_Y_BODY1",
                 "UI_Y_BODY2", "UI_Y_BODY3", "UI_Y_FOOT"]]
        self.assertEqual(rows, sorted(rows), "rows are not in visual order")
        for above, below in zip(rows, rows[1:]):
            self.assertGreaterEqual(
                below - above, CELL_HEIGHT,
                "rows at y=%d and y=%d overlap: a %dpx glyph needs %dpx"
                % (above, below, CELL_HEIGHT, CELL_HEIGHT))

    def test_the_rule_sits_between_the_header_and_the_body(self):
        rule = define(self.ui, "UI_Y_RULE")
        self.assertGreater(rule, define(self.ui, "UI_Y_IFACE"))
        self.assertLess(rule, define(self.ui, "UI_Y_BODY0"))

    def test_the_body_columns_do_not_collide(self):
        # The left column holds the mesh counts, the right this board's own
        # history. The widest left label is "NOM:" plus a count.
        left_widest = len("NOM:") + 4      # room for four digits and a '+'
        right_column = 64                  # the x the right column starts at
        self.assertLessEqual(
            left_widest * CELL_WIDTH, right_column,
            "the left column can run into the right one")

    def test_the_interface_row_fits_beside_the_clock(self):
        # Four badges plus the clock. This is the arithmetic the design's own
        # annotation got wrong: bracketed badges are 16 characters, and with a
        # six-character clock that is 23 on a 21-column line.
        badges = ["LR", "BT", "WF", "EN"]
        gap_px = 4          # ui_badge advances by width + 4
        badge_px = sum(len(b) * CELL_WIDTH + 1 + gap_px for b in badges)
        clock_px = len("00:00z") * CELL_WIDTH
        self.assertLessEqual(
            badge_px + clock_px, PANEL_WIDTH,
            "the interface row overflows: %dpx of badges plus %dpx of clock "
            "exceeds %dpx" % (badge_px, clock_px, PANEL_WIDTH))

    def test_an_inactive_badge_is_not_boxed(self):
        # Boxing the inactive badges too made the row read as one blob on
        # hardware: an outlined badge between two inverted ones has its borders
        # touching their fills.
        badge = self.ui[self.ui.index("inline int ui_badge("):
                        self.ui.index("inline void ui_right_text(")]
        active, inactive = badge.split("} else {", 1)
        self.assertIn("fillRect", active)
        self.assertNotIn("drawRect", inactive)


class PanelSemanticsTests(unittest.TestCase):
    def setUp(self):
        self.status = source("NodeStatus.cpp")

    def test_uptime_is_not_reticulums_logical_clock(self):
        # OS::monotonic_time() carries a persisted offset across reboots. The
        # panel read 21h48m on a board that had been powered four minutes.
        self.assertIn("node_uptime_seconds", self.status)
        uptime = self.status[self.status.index("uint32_t node_uptime_seconds()"):]
        uptime = uptime[:uptime.index("NodeStatusView node_status()")]
        self.assertIn("millis()", uptime)
        self.assertNotIn("monotonic_time", uptime)
        # millis() wraps every 49.7 days and these run for months.
        self.assertIn("wraps", uptime)

    def test_mesh_state_is_not_the_relay_flag(self):
        # transport_enabled is forced false whenever op_mode is not MODE_TNC,
        # which is always on a board with no modem fitted. Gating the mesh
        # indicator on it reported MESH -- on a node with 17 paths and two
        # live radios.
        self.assertIn("s.mesh_on = any_radio && (s.nodes > 0)", self.status)
        self.assertIn("s.relaying", self.status)

    def test_absent_hardware_is_not_reported_as_down(self):
        # A board with no LoRa fitted has nothing wrong with it.
        self.assertIn("!s.lora_present || s.lora_active", self.status)
        self.assertIn("!s.ble_present || s.ble_active", self.status)

    def test_the_census_counts_distinct_destinations(self):
        header = source("NodeStatus.h")
        record = header[header.index("inline void node_census_record("):]
        record = record[:record.index("// --- abnormal restarts")]
        # A node announcing every 30 minutes must not count as a new peer each
        # time.
        self.assertIn("return;", record)
        self.assertIn("c.prefix[i] == prefix", record)
        # And a full table must be visible, not silently short.
        self.assertIn("overflowed", record)


if __name__ == "__main__":
    unittest.main()
