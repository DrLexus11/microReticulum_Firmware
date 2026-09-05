// The 128x64 panel, drawn from docs/ui/ssd1306.
//
// Replaces the inherited RNode screen on boards that have one of our panels.
// That screen was built for a handheld modem -- signal bars, a waterfall, a
// Bluetooth pairing hint -- and says almost nothing about the thing this
// hardware actually is, which is a node in a mesh. This says what the mesh
// looks like from here.
//
// Two departures from the design mockup, both deliberate:
//
//   * The hero render shows roughly two and a half times the text that fits at
//     128x64. The design's own 1:1 block is the buildable one and is what this
//     follows.
//   * `[LR][BT][WF][EN]` plus the clock is 23 characters on a 21-character
//     line. Drawing the badges as filled rectangles rather than bracket
//     characters fits, and is closer to the "inverted white boxes" the design
//     asks for anyway.
#pragma once

#include "NodeStatus.h"
#include "Nav.h"

#if defined(HAS_RNS) && HAS_DISPLAY == true

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// The built-in 5x7 glyph in a 6x8 cell: 21 columns, 8 rows. Org_01 is smaller
// and fits more, but this panel is read across a room, not held.
#define UI_COL 6
#define UI_ROW 8

// Rows are placed by hand rather than by a loop: the header pair is tighter
// than the body so the body gets the space, and a uniform grid wastes the two
// pixels that make the difference between six body rows and seven.
#define UI_Y_TITLE   0
#define UI_Y_IFACE   9
#define UI_Y_RULE   18
#define UI_Y_BODY0  21
#define UI_Y_BODY1  30
#define UI_Y_BODY2  39
#define UI_Y_BODY3  48
#define UI_Y_FOOT   56

// Format an uptime into at most six characters. Days matter once there are
// any; below that, minutes do.
inline void ui_format_uptime(char* out, size_t size, uint32_t seconds) {
  const uint32_t days = seconds / 86400UL;
  const uint32_t hours = (seconds % 86400UL) / 3600UL;
  const uint32_t minutes = (seconds % 3600UL) / 60UL;
  if (days > 0) {
    snprintf(out, size, "%lud%02luh", (unsigned long)days, (unsigned long)hours);
  } else if (hours > 0) {
    snprintf(out, size, "%luh%02lum", (unsigned long)hours, (unsigned long)minutes);
  } else {
    snprintf(out, size, "%lum", (unsigned long)minutes);
  }
}

// A badge: text on an inverted block when active, plain text when merely
// present. Absent hardware is not drawn at all -- a board with no LoRa fitted
// should not show a dead LoRa badge, because nothing is wrong with it.
// Returns the x to continue from.
inline int ui_badge(Adafruit_SSD1306& d, int x, int y, const char* text, bool active) {
  const int width = (int)strlen(text) * UI_COL + 1;
  if (active) {
    d.fillRect(x, y - 1, width, UI_ROW + 1, SSD1306_WHITE);
    d.setTextColor(SSD1306_BLACK);
  } else {
    // Plain text, no outline. Boxing the inactive ones too made the row read
    // as a single blob on hardware -- an outlined badge between two inverted
    // ones has borders touching its neighbours' fills, and at a glance the
    // three merge. The design asks for inverted boxes to mark what is active;
    // everything else is just a label.
    d.setTextColor(SSD1306_WHITE);
  }
  d.setCursor(x + 1, y);
  d.print(text);
  d.setTextColor(SSD1306_WHITE);
  return x + width + 4;
}

inline void ui_right_text(Adafruit_SSD1306& d, int y, const char* text) {
  const int width = (int)strlen(text) * UI_COL;
  d.setCursor(d.width() - width, y);
  d.print(text);
}

// --- page 1: main status -----------------------------------------------------

inline void ui_draw_main(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  char buf[24];

  // Title row. The node name is the one string an operator uses to tell two
  // boards apart, so it gets the left edge and is truncated rather than
  // wrapped.
  d.setCursor(0, UI_Y_TITLE);
  const int name_room = 13;   // leaves room for the badges and the page counter
  snprintf(buf, sizeof(buf), "%.*s", name_room, s.name && s.name[0] ? s.name : "UNNAMED");
  d.print(buf);

  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(page + 1), (unsigned)NAV_PAGE_COUNT);
  ui_right_text(d, UI_Y_TITLE, buf);

  // Service badges sit just left of the page counter.
  int bx = d.width() - (int)strlen(buf) * UI_COL - 2;
  if (s.propagation) { bx -= (2 * UI_COL + 3); ui_badge(d, bx, UI_Y_TITLE, "PR", true); }
  if (s.rrc_hub)     { bx -= (3 * UI_COL + 3); ui_badge(d, bx, UI_Y_TITLE, "RRC", true); }

  // Interface row.
  int ix = 0;
  if (s.lora_present)   ix = ui_badge(d, ix, UI_Y_IFACE, "LR", s.lora_active);
  if (s.ble_present)    ix = ui_badge(d, ix, UI_Y_IFACE, "BT", s.ble_active);
  if (s.wifi_present)   ix = ui_badge(d, ix, UI_Y_IFACE, "WF", s.wifi_active);
  if (s.espnow_present) ix = ui_badge(d, ix, UI_Y_IFACE, "EN", s.espnow_active);

  // Network time, or an honest absence. A clock that silently shows uptime
  // where UTC belongs is worse than one that admits it has none.
  if (s.time_known) {
    const time_t seconds = (time_t)(s.unix_ms / 1000ULL);
    struct tm utc {};
    if (gmtime_r(&seconds, &utc) != nullptr) {
      snprintf(buf, sizeof(buf), "%02d:%02dz", utc.tm_hour, utc.tm_min);
    } else {
      snprintf(buf, sizeof(buf), "--:--z");
    }
  } else {
    snprintf(buf, sizeof(buf), "NO UTC");
  }
  ui_right_text(d, UI_Y_IFACE, buf);

  d.drawFastHLine(0, UI_Y_RULE, d.width(), SSD1306_WHITE);

  // Body: the mesh on the left, this board's own history on the right. The
  // census counts can be short once its table fills, and says so with a '+'
  // rather than quietly reporting a number it knows is wrong.
  const char* more = s.census_full ? "+" : "";
  d.setCursor(0, UI_Y_BODY0);
  d.printf("P:%u%s", (unsigned)s.peers, more);
  d.setCursor(0, UI_Y_BODY1);
  d.printf("N:%u", (unsigned)s.nodes);
  d.setCursor(0, UI_Y_BODY2);
  d.printf("R:%u%s", (unsigned)s.relays, more);
  d.setCursor(0, UI_Y_BODY3);
  d.printf("NOM:%u%s", (unsigned)s.nomad, more);

  const int right = 64;
  char uptime[8];
  ui_format_uptime(uptime, sizeof(uptime), s.uptime_s);
  d.setCursor(right, UI_Y_BODY0);
  d.printf("UP:%s", uptime);
  d.setCursor(right, UI_Y_BODY1);
  d.printf("BOOT:%lu", (unsigned long)s.boots);
  d.setCursor(right, UI_Y_BODY2);
  d.printf("CR:%lu", (unsigned long)s.crashes);
  d.setCursor(right, UI_Y_BODY3);
  d.printf("PN:%lu", (unsigned long)s.panics);

  // Footer. Interface health, not traffic: a quiet mesh at 04:00 is not a
  // fault, and a panel that cries wolf overnight gets ignored by morning.
  char foot[12];
  if (s.interfaces_ok) {
    snprintf(foot, sizeof(foot), "IF OK");
  } else {
    snprintf(foot, sizeof(foot), "IF:%s", s.down_interface ? s.down_interface : "??");
  }
  d.setCursor(0, UI_Y_FOOT);
  d.print(foot);
  d.setCursor(6 * UI_COL, UI_Y_FOOT);
  d.print(s.mesh_on ? "MESH ON" : "MESH --");
  ui_right_text(d, UI_Y_FOOT, "<P|N>");
}

// --- pages 2-4 ---------------------------------------------------------------
//
// Only the main screen is designed. These are readable interpretations of the
// titles the design gives them, built from data that already exists, and are
// meant to be replaced once the rest is drawn rather than defended.

inline void ui_draw_placeholder(Adafruit_SSD1306& d, const NodeStatusView& s,
                                uint8_t page, const char* title) {
  char buf[24];
  d.setCursor(0, UI_Y_TITLE);
  d.print(title);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(page + 1), (unsigned)NAV_PAGE_COUNT);
  ui_right_text(d, UI_Y_TITLE, buf);
  d.drawFastHLine(0, UI_Y_RULE, d.width(), SSD1306_WHITE);
  (void)s;
}

inline void ui_draw_rrc(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  ui_draw_placeholder(d, s, page, "RRC HUB");
  d.setCursor(0, UI_Y_BODY0);
  d.print(s.rrc_hub ? "HUB: RUNNING" : "HUB: OFF");
  d.setCursor(0, UI_Y_BODY1);
  d.print(s.propagation ? "PROP: ON" : "PROP: OFF");
  d.setCursor(0, UI_Y_FOOT);
  d.print("NOT YET DESIGNED");
}

inline void ui_draw_peers(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  ui_draw_placeholder(d, s, page, "PEERS/SITES");
  d.setCursor(0, UI_Y_BODY0);
  d.printf("PEERS   %u", (unsigned)s.peers);
  d.setCursor(0, UI_Y_BODY1);
  d.printf("RELAYS  %u", (unsigned)s.relays);
  d.setCursor(0, UI_Y_BODY2);
  d.printf("NOMAD   %u", (unsigned)s.nomad);
  d.setCursor(0, UI_Y_BODY3);
  d.printf("PATHS   %u", (unsigned)s.nodes);
  d.setCursor(0, UI_Y_FOOT);
  d.print(s.census_full ? "CENSUS FULL" : "NOT YET DESIGNED");
}

inline void ui_draw_interfaces(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  ui_draw_placeholder(d, s, page, "INTERFACES");
  int y = UI_Y_BODY0;
  if (s.lora_present)   { d.setCursor(0, y); d.printf("LORA    %s", s.lora_active ? "UP" : "DOWN"); y += 9; }
  if (s.ble_present)    { d.setCursor(0, y); d.printf("BLE     %s", s.ble_active ? "UP" : "DOWN"); y += 9; }
  if (s.wifi_present)   { d.setCursor(0, y); d.printf("WIFI    %s", s.wifi_active ? "UP" : "DOWN"); y += 9; }
  if (s.espnow_present) { d.setCursor(0, y); d.printf("ESPNOW  %s", s.espnow_active ? "UP" : "IDLE"); y += 9; }
  d.setCursor(0, UI_Y_FOOT);
  if (s.time_known) d.printf("UTC STRATUM %u", (unsigned)s.stratum);
  else              d.print("UTC UNKNOWN");
}

// --- entry point -------------------------------------------------------------

// Dump the framebuffer so a panel can be reviewed without being looked at.
//
// Iterating on a 128x64 layout otherwise means asking someone to lean over the
// bench and describe it, which is slow and lossy. This prints the 1024-byte
// buffer as hex between markers; tools/panel_render.py turns that back into a
// PNG. Flag-gated and off by default -- it writes to the same serial line the
// KISS host uses.
#if defined(NODE_PANEL_DUMP)
#ifndef NODE_PANEL_DUMP_MS
#define NODE_PANEL_DUMP_MS 10000UL
#endif
inline void display_ui_dump(Adafruit_SSD1306& d) {
  static uint32_t last_dump = 0;
  if (millis() - last_dump < NODE_PANEL_DUMP_MS) return;
  last_dump = millis();
  const NodeStatusView st = node_status();
  Serial.printf("[panel] lora=%d/%d ble=%d/%d wifi=%d/%d espnow=%d/%d down=%s ok=%d mesh=%d\n",
                st.lora_present, st.lora_active, st.ble_present, st.ble_active,
                st.wifi_present, st.wifi_active, st.espnow_present, st.espnow_active,
                st.down_interface ? st.down_interface : "-", st.interfaces_ok, st.mesh_on);
  const uint8_t* buffer = d.getBuffer();
  if (buffer == nullptr) return;
  const size_t length = ((size_t)d.width() * (size_t)d.height()) / 8;
  Serial.printf("\n[panel] %ux%u begin\n", (unsigned)d.width(), (unsigned)d.height());
  for (size_t i = 0; i < length; ++i) {
    Serial.printf("%02x", buffer[i]);
    if ((i % 32) == 31) Serial.print('\n');
  }
  Serial.println("[panel] end");
}
#else
inline void display_ui_dump(Adafruit_SSD1306&) {}
#endif

inline void display_ui_render(Adafruit_SSD1306& d) {
  const NodeStatusView s = node_status();
  const uint8_t page = nav_page();

  d.setFont(NULL);       // the built-in 6x8 cell, not Org_01
  d.setTextSize(1);
  d.setTextWrap(false);
  d.setTextColor(SSD1306_WHITE);

  switch (page) {
    case NAV_PAGE_RRC:        ui_draw_rrc(d, s, page); break;
    case NAV_PAGE_PEERS:      ui_draw_peers(d, s, page); break;
    case NAV_PAGE_INTERFACES: ui_draw_interfaces(d, s, page); break;
    case NAV_PAGE_MAIN:
    default:                  ui_draw_main(d, s, page); break;
  }

  display_ui_dump(d);
}

#endif
