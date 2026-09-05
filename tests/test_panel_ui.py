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


def define(text, name, _seen=None):
    """Resolve a #define to a number, following simple arithmetic on other
    defines. The layout constants are expressed in terms of each other -- the
    column gutters are the split minus a margin -- so reading only integer
    literals would mean the tests could not see them at all."""
    match = re.search(r"^#define\s+%s\s+(.+?)\s*$" % re.escape(name), text,
                      re.MULTILINE)
    if match is None:
        raise AssertionError("no #define for %s" % name)
    expression = match.group(1)
    _seen = set(_seen or ())
    if name in _seen:
        raise AssertionError("%s is defined in terms of itself" % name)
    _seen.add(name)
    for symbol in sorted(set(re.findall(r"[A-Za-z_][A-Za-z0-9_]*", expression)),
                         key=len, reverse=True):
        expression = expression.replace(
            symbol, str(define(text, symbol, _seen)))
    if not re.fullmatch(r"[-+*/()\d\s]+", expression):
        raise AssertionError("%s is not arithmetic: %s" % (name, match.group(1)))
    return int(eval(expression))  # noqa: S307 - arithmetic only, checked above


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

    def test_the_design_rules_are_all_present_and_in_order(self):
        # The design carries four: under the identity row, under the badge row,
        # between the two columns, and above the footer. They are what stop six
        # rows of 6px text reading as a wall, and three of them were missing.
        title = define(self.ui, "UI_Y_TITLE")
        top = define(self.ui, "UI_Y_RULE_TOP")
        iface = define(self.ui, "UI_Y_IFACE")
        mid = define(self.ui, "UI_Y_RULE_MID")
        body0 = define(self.ui, "UI_Y_BODY0")
        bot = define(self.ui, "UI_Y_RULE_BOT")
        foot = define(self.ui, "UI_Y_FOOT")
        self.assertLess(title, top)
        self.assertLess(top, iface)
        self.assertLess(iface, mid)
        self.assertLess(mid, body0)
        self.assertLess(define(self.ui, "UI_Y_BODY3"), bot)
        self.assertLess(bot, foot)
        # And the vertical one between the columns.
        split = define(self.ui, "UI_X_SPLIT")
        self.assertLess(define(self.ui, "UI_X_LEFT_END"), split)
        self.assertLess(split, define(self.ui, "UI_X_RIGHT"))
        self.assertIn("drawFastVLine(UI_X_SPLIT", self.ui)

    def test_every_interface_badge_is_always_drawn(self):
        # A badge that disappears when its hardware is absent reads as a
        # rendering fault. Absent is struck through instead, so the row never
        # shifts and "not fitted" is visibly different from "fitted and down".
        main = self.ui[self.ui.index("inline void ui_draw_main("):
                       self.ui.index("// --- pages 2-4")]
        for badge in ('"LR"', '"BT"', '"WF"', '"EN"'):
            self.assertIn(badge, main)
        self.assertNotIn("if (s.lora_present)", main)
        self.assertIn("UI_BADGE_ABSENT", self.ui)

    def test_the_feature_badges_are_always_drawn(self):
        main = self.ui[self.ui.index("inline void ui_draw_main("):
                       self.ui.index("// --- pages 2-4")]
        self.assertIn('"RRC"', main)
        self.assertIn('"PR"', main)

    def test_the_page_hint_does_not_take_footer_space(self):
        # 128x64 has no room for a legend telling you the buttons exist.
        self.assertNotIn("<P|N>", self.ui)

    def test_content_is_inset_from_the_panel_edges(self):
        # The design insets its content rather than running text against the
        # glass, and it is most of what stops the panel looking poured in.
        margin = define(self.ui, "UI_MARGIN")
        self.assertGreater(margin, 0)
        self.assertEqual(define(self.ui, "UI_X_LEFT"), margin)
        self.assertEqual(define(self.ui, "UI_X_RIGHT_EDGE"), PANEL_WIDTH - margin)

    def test_badges_clear_the_rules_above_and_below(self):
        # A fill that butts against both rules merges with them into a band.
        top = define(self.ui, "UI_Y_RULE_TOP")
        row = define(self.ui, "UI_Y_IFACE")
        bottom = define(self.ui, "UI_Y_RULE_MID")
        height = define(self.ui, "UI_BADGE_H")
        self.assertGreater(row, top, "the badge fill touches the rule above")
        self.assertLess(row + height, bottom,
                        "the badge fill touches the rule below")

    def test_the_columns_are_padded_off_the_vertical_rule(self):
        split = define(self.ui, "UI_X_SPLIT")
        self.assertLess(define(self.ui, "UI_X_LEFT_END"), split)
        self.assertGreater(define(self.ui, "UI_X_RIGHT"), split)

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

    def test_trouble_inverts_the_whole_footer(self):
        # The one thing on this panel readable across a room without reading
        # it: a white bar along the bottom means go and look.
        band = self.ui[self.ui.index("inline void ui_draw_footer_band("):
                       self.ui.index("// --- page 1: main status")]
        self.assertIn("const bool trouble", band)
        self.assertIn("canvas.fillScreen(trouble ? 1 : 0)", band)

    def test_the_alarm_does_not_cycle_with_the_content(self):
        # A status light that blinks the problem away every few seconds is
        # worse than one that never lit. The background is set from `trouble`
        # once per draw, outside the slot selection.
        band = self.ui[self.ui.index("inline void ui_draw_footer_band("):
                       self.ui.index("// --- page 1: main status")]
        alarm = band.index("canvas.fillScreen(trouble ? 1 : 0)")
        # Every slot is drawn after the background is laid down, so all of them
        # inherit it.
        self.assertLess(alarm, band.index("ui_footer_draw_slot"))

    def test_the_footer_rolls_one_pixel_per_frame(self):
        # A pixel is the smallest step this display has, so a 1px roll is the
        # smoothest motion it can produce; interpolating between pixel
        # positions could only add stutter.
        band = self.ui[self.ui.index("inline void ui_draw_footer_band("):
                       self.ui.index("// --- page 1: main status")]
        self.assertIn("st.roll++", band)
        self.assertIn("st.roll >= UI_FOOT_BAND_H", band)

    def test_the_rolling_footer_is_clipped_to_its_band(self):
        # Drawn straight onto the panel, the outgoing line would smear up
        # through the body rows. Blitting an offscreen canvas is what clips it.
        band = self.ui[self.ui.index("inline void ui_draw_footer_band("):
                       self.ui.index("// --- page 1: main status")]
        self.assertIn("GFXcanvas1 canvas", band)
        self.assertIn("drawBitmap(0, UI_FOOT_BAND_Y", band)

    def test_a_stale_clock_shows_uptime_not_a_time(self):
        # "OLD 18:51" is worse than no clock: a plausible time on the glass
        # with the lie in three characters that are easy to stop seeing.
        main = self.ui[self.ui.index("inline void ui_draw_main("):
                       self.ui.index("// --- pages 2-4")]
        self.assertIn("if (s.time_current)", main)
        self.assertIn('"UP %s"', main)
        status = source("NodeStatus.cpp")
        self.assertIn("WallTimeSource::PERSISTED", status)

    def test_the_clock_shows_it_is_ticking(self):
        # Ten characters fit beside the badges and "NTP 18:51:22z" is
        # thirteen, so the protocol name wins and the colon carries the
        # liveliness -- as every digital clock has since they were invented.
        main = self.ui[self.ui.index("inline void ui_draw_main("):
                       self.ui.index("// --- pages 2-4")]
        self.assertIn("tick ? ':' : ' '", main)
        # The seconds go where a whole line is free.
        build = self.ui[self.ui.index("inline uint8_t ui_footer_build("):
                        self.ui.index("inline void ui_footer_draw_slot(")]
        self.assertIn("utc.tm_sec", build)

    def test_the_clock_fits_beside_the_badges(self):
        # Four badges from the left margin, then the widest clock string.
        badge_px = 4 * (2 * CELL_WIDTH + 1) + 3 * define(self.ui, "UI_BADGE_GAP")
        free = (PANEL_WIDTH - define(self.ui, "UI_MARGIN")) - (
            define(self.ui, "UI_MARGIN") + badge_px)
        for text in ("NTP 18:51z", "UP 21h48m"):
            self.assertLessEqual(
                len(text) * CELL_WIDTH, free,
                "%r needs %dpx of the %dpx beside the badges"
                % (text, len(text) * CELL_WIDTH, free))

    def test_the_footer_only_offers_slots_that_apply(self):
        # A board with no ESP-NOW fitted should not cycle through empty frames
        # about it, and "idle" every four seconds is noise.
        build = self.ui[self.ui.index("inline uint8_t ui_footer_build("):
                        self.ui.index("inline void ui_footer_draw_slot(")]
        self.assertIn("if (s.espnow_present)", build)
        self.assertIn("interesting", build)

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
        self.assertIn("s.mesh_on = any_radio && (s.paths > 0)", self.status)

    def test_nodes_counts_identities_not_destinations(self):
        # Every node announces several destinations -- transport, probe,
        # management, nomadnet, lxmf delivery, propagation -- so the path table
        # reads four to eight times the number of actual devices. Sixteen paths
        # on the bench was four boards, under a label that said NODES.
        self.assertIn("s.nodes = c.counts[NODE_CENSUS_NODE]", self.status)
        self.assertIn("s.paths = (uint16_t)RNS::Transport::new_path_table().size()",
                      self.status)
        header = source("NodeStatus.h")
        self.assertIn("NODE_CENSUS_NODE", header)
        # Keyed on the identity, or one node announcing six destinations counts
        # as six nodes.
        self.assertIn("announced_identity.hash()", self.status)
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
