// The 128x64 panel, drawn from docs/ui/ssd1306.
//
// Replaces the inherited RNode screen on boards that have one of our panels.
// That screen was built for a handheld modem -- signal bars, a waterfall, a
// Bluetooth pairing hint -- and says almost nothing about the thing this
// hardware actually is, which is a node in a mesh.
//
// The design's rules are load-bearing and are followed: one under the identity
// row, one under the badge row, one between the two key/value columns, and one
// above the footer. They are what stop six rows of 6px text reading as a wall.
//
// One departure, measured on hardware: the design's bracketed badges
// (`[LR][BT][WF][EN]`) plus the clock come to 23 characters on a 21-character
// line. Badges are drawn as fills instead, which is what "inverted white boxes
// indicate active broadcast" asks for anyway.
#pragma once

#include "NodeStatus.h"
#include "Nav.h"

#if defined(HAS_RNS) && HAS_DISPLAY == true

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// The built-in 5x7 glyph in a 6x8 cell: 21 columns, 8 rows. Org_01 is smaller
// and would fit more, but this panel is read across a room, not held.
#define UI_COL 6
#define UI_ROW 8

// Rows and rules, hand-placed: the rules cost three scanlines that a uniform
// grid would have spent on padding.
#define UI_Y_TITLE      0
#define UI_Y_RULE_TOP   8
#define UI_Y_IFACE     10
#define UI_Y_RULE_MID  18
#define UI_Y_BODY0     20
#define UI_Y_BODY1     28
#define UI_Y_BODY2     36
#define UI_Y_BODY3     44
#define UI_Y_RULE_BOT  53
#define UI_Y_FOOT      55

// Margins. The design insets its content from the panel edge and from the
// rules rather than running text against them, and it is what stops the whole
// thing looking like it was poured in. Everything is placed off these.
#define UI_MARGIN       3
#define UI_X_LEFT       UI_MARGIN
#define UI_X_RIGHT_EDGE (128 - UI_MARGIN)

// Badges sit one scanline clear of the rules above and below, rather than
// touching them. Seven pixels of fill in a nine-pixel band.
#define UI_BADGE_H      7
#define UI_BADGE_GAP    3

// The vertical rule and the two columns it separates, each padded off it.
#define UI_X_SPLIT     63
#define UI_X_LEFT_END  (UI_X_SPLIT - 3)
#define UI_X_RIGHT     (UI_X_SPLIT + 4)

// Three states, because "no LoRa fitted" and "LoRa fitted and dead" are
// different problems, and neither of them is "LoRa working".
enum UiBadgeState : uint8_t {
  UI_BADGE_ABSENT = 0,
  UI_BADGE_IDLE = 1,
  UI_BADGE_ACTIVE = 2,
};

inline UiBadgeState ui_badge_state(bool present, bool active) {
  if (!present) return UI_BADGE_ABSENT;
  return active ? UI_BADGE_ACTIVE : UI_BADGE_IDLE;
}

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

// Draws one badge, returning the x to continue from.
//
// Only the active state is boxed. Outlining the idle ones too was tried and
// reverted: on the panel an outlined badge between two filled ones has its
// borders touching their fills, and the three read as a single blob.
inline int ui_badge(Adafruit_SSD1306& d, int x, int y, const char* text,
                    UiBadgeState state) {
  const int width = (int)strlen(text) * UI_COL + 1;
  if (state == UI_BADGE_ACTIVE) {
    // Exactly the glyph's height, so the fill clears the rules above and
    // below instead of butting against them.
    d.fillRect(x, y, width, UI_BADGE_H, SSD1306_WHITE);
    d.setTextColor(SSD1306_BLACK);
  } else {
    d.setTextColor(SSD1306_WHITE);
  }
  d.setCursor(x + 1, y);
  d.print(text);
  d.setTextColor(SSD1306_WHITE);
  if (state == UI_BADGE_ABSENT) {
    // Struck through. The badge still occupies its place so the row never
    // shifts, but the hardware behind it is not there -- a badge that simply
    // vanishes reads as a rendering fault.
    d.drawFastHLine(x, y + 3, width, SSD1306_WHITE);
  }
  return x + width + UI_BADGE_GAP;
}

inline void ui_right_text(Adafruit_SSD1306& d, int right_edge, int y, const char* text) {
  d.setCursor(right_edge - (int)strlen(text) * UI_COL, y);
  d.print(text);
}

// A label on the left of a column with its value hard against the right, which
// is what makes a column of numbers scannable rather than merely present.
inline void ui_pair(Adafruit_SSD1306& d, int x, int right_edge, int y,
                    const char* label, const char* value) {
  d.setCursor(x, y);
  d.print(label);
  ui_right_text(d, right_edge, y, value);
}

// --- page 1: main status -----------------------------------------------------

inline void ui_draw_main(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  char buf[24];
  char value[12];

  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(page + 1), (unsigned)NAV_PAGE_COUNT);
  ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_TITLE, buf);

  // Service badges, always drawn: whether a feature is off is information, and
  // a row whose contents move about is harder to read than one that does not.
  int bx = UI_X_RIGHT_EDGE - (int)strlen(buf) * UI_COL - UI_BADGE_GAP;
  bx -= (2 * UI_COL + 1);
  ui_badge(d, bx, UI_Y_TITLE, "PR", ui_badge_state(true, s.propagation));
  bx -= (3 * UI_COL + 1) + UI_BADGE_GAP;
  ui_badge(d, bx, UI_Y_TITLE, "RRC", ui_badge_state(true, s.rrc_hub));

  const int name_columns = (bx - UI_X_LEFT - UI_BADGE_GAP) / UI_COL;
  snprintf(buf, sizeof(buf), "%.*s", name_columns > 0 ? name_columns : 1,
           s.name && s.name[0] ? s.name : "UNNAMED");
  d.setCursor(UI_X_LEFT, UI_Y_TITLE);
  d.print(buf);

  d.drawFastHLine(0, UI_Y_RULE_TOP, d.width(), SSD1306_WHITE);

  // Interface row. Every badge is always drawn, LoRa included on a board that
  // has none.
  int ix = UI_X_LEFT;
  ix = ui_badge(d, ix, UI_Y_IFACE, "LR", ui_badge_state(s.lora_present, s.lora_active));
  ix = ui_badge(d, ix, UI_Y_IFACE, "BT", ui_badge_state(s.ble_present, s.ble_active));
  ix = ui_badge(d, ix, UI_Y_IFACE, "WF", ui_badge_state(s.wifi_present, s.wifi_active));
  ix = ui_badge(d, ix, UI_Y_IFACE, "EN", ui_badge_state(s.espnow_present, s.espnow_active));
  (void)ix;

  // Network time, or an honest absence. A clock quietly showing uptime where
  // UTC belongs is worse than one that admits it has none.
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
  ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_IFACE, buf);

  // Where the clock came from, in the design's "NTC" slot but naming the
  // actual provenance. A clock restored from storage and one disciplined by
  // NTP read identically otherwise, and only one of them is worth trusting --
  // which is the whole argument the time work has been making.
  if (s.time_known && s.time_source && s.time_source[0]) {
    // Plain text, as the design has "NTC": the badge row is already carrying
    // four inverted blocks and a fifth turns the row into noise. This is a
    // label on the clock, not another piece of hardware.
    const int tag_x = UI_X_RIGHT_EDGE - (int)strlen(buf) * UI_COL
                      - UI_BADGE_GAP - (int)strlen(s.time_source) * UI_COL;
    d.setCursor(tag_x, UI_Y_IFACE);
    d.print(s.time_source);
  }

  d.drawFastHLine(0, UI_Y_RULE_MID, d.width(), SSD1306_WHITE);

  // The mesh on the left, this board's own history on the right, with the rule
  // between them that makes two columns two columns.
  d.drawFastVLine(UI_X_SPLIT, UI_Y_RULE_MID + 1,
                  UI_Y_RULE_BOT - UI_Y_RULE_MID - 1, SSD1306_WHITE);

  // A '+' where the census table has filled: the count has stopped rising, and
  // saying so beats reporting a number known to be short.
  const char* more = s.census_full ? "+" : "";
  snprintf(value, sizeof(value), "%u%s", (unsigned)s.peers, more);
  ui_pair(d, UI_X_LEFT, UI_X_LEFT_END, UI_Y_BODY0, "PEERS", value);
  snprintf(value, sizeof(value), "%u%s", (unsigned)s.nodes, more);
  ui_pair(d, UI_X_LEFT, UI_X_LEFT_END, UI_Y_BODY1, "NODES", value);
  snprintf(value, sizeof(value), "%u%s", (unsigned)s.relays, more);
  ui_pair(d, UI_X_LEFT, UI_X_LEFT_END, UI_Y_BODY2, "RELAY", value);
  snprintf(value, sizeof(value), "%u%s", (unsigned)s.nomad, more);
  ui_pair(d, UI_X_LEFT, UI_X_LEFT_END, UI_Y_BODY3, "NOMAD", value);

  char uptime[10];
  ui_format_uptime(uptime, sizeof(uptime), s.uptime_s);
  ui_pair(d, UI_X_RIGHT, UI_X_RIGHT_EDGE, UI_Y_BODY0, "UP", uptime);
  snprintf(value, sizeof(value), "%lu", (unsigned long)s.boots);
  ui_pair(d, UI_X_RIGHT, UI_X_RIGHT_EDGE, UI_Y_BODY1, "BOOT", value);
  snprintf(value, sizeof(value), "%lu", (unsigned long)s.crashes);
  ui_pair(d, UI_X_RIGHT, UI_X_RIGHT_EDGE, UI_Y_BODY2, "CRASH", value);
  snprintf(value, sizeof(value), "%lu", (unsigned long)s.panics);
  ui_pair(d, UI_X_RIGHT, UI_X_RIGHT_EDGE, UI_Y_BODY3, "PANIC", value);

  d.drawFastHLine(0, UI_Y_RULE_BOT, d.width(), SSD1306_WHITE);

  // Health bar.
  //
  // The whole strip inverts when something is wrong, which is the one thing on
  // this panel that is readable from across a room without reading it: a white
  // bar along the bottom means go and look. "IF:WF MESH ON" said as much in
  // two half-sentences that had to be parsed first.
  const bool trouble = !s.mesh_on || !s.interfaces_ok;
  if (trouble) {
    d.fillRect(0, UI_Y_RULE_BOT + 1, d.width(), d.height() - UI_Y_RULE_BOT - 1,
               SSD1306_WHITE);
    d.setTextColor(SSD1306_BLACK);
  }
  d.setCursor(UI_X_LEFT, UI_Y_FOOT);
  d.print(s.mesh_on ? "MESH ON" : "NO MESH");
  if (s.interfaces_ok) {
    ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_FOOT, "ALL OK");
  } else {
    snprintf(buf, sizeof(buf), "%s DOWN", s.down_interface ? s.down_interface : "IF");
    ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_FOOT, buf);
  }
  d.setTextColor(SSD1306_WHITE);
}

// --- pages 2-4 ---------------------------------------------------------------
//
// Only the main screen is designed. These are readable interpretations of the
// titles the design gives them, built from data that already exists, and are
// meant to be replaced once the rest is drawn rather than defended.

inline void ui_draw_header(Adafruit_SSD1306& d, uint8_t page, const char* title) {
  char buf[12];
  d.setCursor(UI_X_LEFT, UI_Y_TITLE);
  d.print(title);
  snprintf(buf, sizeof(buf), "%u/%u", (unsigned)(page + 1), (unsigned)NAV_PAGE_COUNT);
  ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_TITLE, buf);
  d.drawFastHLine(0, UI_Y_RULE_TOP, d.width(), SSD1306_WHITE);
}

inline void ui_draw_footer(Adafruit_SSD1306& d, const char* text) {
  d.drawFastHLine(0, UI_Y_RULE_BOT, d.width(), SSD1306_WHITE);
  d.setCursor(UI_X_LEFT, UI_Y_FOOT);
  d.print(text);
}

inline void ui_draw_rrc(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  ui_draw_header(d, page, "RRC HUB");
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY0, "HUB", s.rrc_hub ? "RUNNING" : "OFF");
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY1, "PROPAGATION", s.propagation ? "ON" : "OFF");
  ui_draw_footer(d, "NOT YET DESIGNED");
}

inline void ui_draw_peers(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  char value[12];
  ui_draw_header(d, page, "PEERS / SITES");
  snprintf(value, sizeof(value), "%u", (unsigned)s.peers);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY0, "PEERS", value);
  snprintf(value, sizeof(value), "%u", (unsigned)s.relays);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY1, "RELAYS", value);
  snprintf(value, sizeof(value), "%u", (unsigned)s.nomad);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY2, "NOMAD SITES", value);
  snprintf(value, sizeof(value), "%u", (unsigned)s.paths);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY3, "PATHS KNOWN", value);
  ui_draw_footer(d, s.census_full ? "CENSUS TABLE FULL" : "SEEN SINCE BOOT");
}

inline void ui_draw_interfaces(Adafruit_SSD1306& d, const NodeStatusView& s, uint8_t page) {
  ui_draw_header(d, page, "INTERFACES");
  const char* absent = "NOT FITTED";
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY0, "LORA",
          s.lora_present ? (s.lora_active ? "UP" : "DOWN") : absent);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY1, "BLE",
          s.ble_present ? (s.ble_active ? "UP" : "DOWN") : absent);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY2, "WIFI",
          s.wifi_present ? (s.wifi_active ? "UP" : "DOWN") : absent);
  ui_pair(d, UI_X_LEFT, UI_X_RIGHT_EDGE, UI_Y_BODY3, "ESP-NOW",
          s.espnow_present ? (s.espnow_active ? "PEERED" : "ALONE") : absent);
  if (s.relaying)        ui_draw_footer(d, "RELAYING FOR OTHERS");
  else if (s.time_known) ui_draw_footer(d, "UTC OK");
  else                   ui_draw_footer(d, "NO UTC");
}

// --- framebuffer dump --------------------------------------------------------
//
// Iterating on a 128x64 layout otherwise means asking someone to lean over the
// bench and describe it. This prints the 1024-byte buffer as hex between
// markers; tools/panel_render.py turns it back into a picture. Flag-gated and
// off by default -- it writes to the serial line the KISS host uses.
#if defined(NODE_PANEL_DUMP)
#ifndef NODE_PANEL_DUMP_MS
#define NODE_PANEL_DUMP_MS 10000UL
#endif
inline void display_ui_dump(Adafruit_SSD1306& d) {
  static uint32_t last_dump = 0;
  if (millis() - last_dump < NODE_PANEL_DUMP_MS) return;
  last_dump = millis();
  const NodeStatusView st = node_status();
  Serial.printf("\n[panel] lora=%d/%d ble=%d/%d wifi=%d/%d espnow=%d/%d "
                "down=%s ok=%d mesh=%d peers=%u nodes=%u relays=%u nomad=%u\n",
                st.lora_present, st.lora_active, st.ble_present, st.ble_active,
                st.wifi_present, st.wifi_active, st.espnow_present, st.espnow_active,
                st.down_interface ? st.down_interface : "-", st.interfaces_ok,
                st.mesh_on, (unsigned)st.peers, (unsigned)st.nodes,
                (unsigned)st.relays, (unsigned)st.nomad);
  const uint8_t* buffer = d.getBuffer();
  if (buffer == nullptr) return;
  const size_t length = ((size_t)d.width() * (size_t)d.height()) / 8;
  Serial.printf("[panel] %ux%u begin\n", (unsigned)d.width(), (unsigned)d.height());
  for (size_t i = 0; i < length; ++i) {
    Serial.printf("%02x", buffer[i]);
    if ((i % 32) == 31) Serial.print('\n');
  }
  Serial.println("[panel] end");
}
#else
inline void display_ui_dump(Adafruit_SSD1306&) {}
#endif

// --- entry point -------------------------------------------------------------

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
