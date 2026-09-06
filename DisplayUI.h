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

// --- the footer, which is the only part of this panel that moves -------------
//
// A still panel hides the state that changes. On a board whose only way back
// to the mesh is a channel sweep, "scanning channel 7" and "attached on 9" are
// the difference between waiting and intervening, and neither fits beside the
// health summary.
//
// So the footer cycles. Four seconds a slot, then the outgoing line rolls up
// and out while the incoming one rolls in beneath it -- the departure-board
// idiom, which people read without being taught.
//
// One pixel per rendered frame, deliberately. The panel refreshes at 7fps and
// a pixel is the smallest step this display has, so a 1px roll is the smoothest
// motion it can physically produce; easing between pixel positions would only
// add stutter. Ten pixels of band at 7fps is a roll of about 1.4 seconds,
// which reads as unhurried rather than twitchy beside a bed.
//
// The alarm does not cycle. When something is wrong the whole band stays
// inverted through every slot, so the warning is continuous while the detail
// underneath it rotates. A status light that blinks away the problem every few
// seconds is worse than one that never lit.

#define UI_FOOT_BAND_Y  (UI_Y_RULE_BOT + 1)
#define UI_FOOT_BAND_H  (64 - UI_Y_RULE_BOT - 1)
#define UI_FOOT_HOLD_MS 4000UL
#define UI_FOOT_SLOTS   4

struct UiFooterSlot {
  char left[16];
  char right[12];
  bool used;
};

struct UiFooterState {
  uint8_t current = 0;
  uint8_t next = 0;
  int8_t roll = -1;            // -1 while holding, else pixels rolled so far
  uint32_t held_since = 0;
};

inline UiFooterState& ui_footer_state() {
  static UiFooterState state;
  return state;
}

// Build the slots that apply to this node. A board with no ESP-NOW fitted
// should not cycle through two empty frames about it.
inline uint8_t ui_footer_build(const NodeStatusView& s, UiFooterSlot* slots) {
  uint8_t count = 0;

  // Always first: is this thing working.
  UiFooterSlot& health = slots[count++];
  snprintf(health.left, sizeof(health.left), "%s", s.mesh_on ? "MESH ON" : "NO MESH");
  if (s.interfaces_ok) {
    snprintf(health.right, sizeof(health.right), "ALL OK");
  } else {
    snprintf(health.right, sizeof(health.right), "%s DOWN",
             s.down_interface ? s.down_interface : "IF");
  }

  if (s.espnow_present) {
    UiFooterSlot& link = slots[count++];
    snprintf(link.left, sizeof(link.left), "EN CH%u", (unsigned)s.espnow_channel);
    if (s.espnow_peers == 0) {
      snprintf(link.right, sizeof(link.right), "NO PEERS");
    } else {
      snprintf(link.right, sizeof(link.right), "%u PEER%s",
               (unsigned)s.espnow_peers, s.espnow_peers == 1 ? "" : "S");
    }

    // Only while the state machine has something to say. "idle" every four
    // seconds is noise.
    const bool interesting = s.recovery_active || s.recovery_pinned || s.recovery_failed;
    if (interesting) {
      UiFooterSlot& rec = slots[count++];
      snprintf(rec.left, sizeof(rec.left), "REC %.9s",
               s.recovery_state ? s.recovery_state : "?");
      // The channel, not the state word again: "REC pinned / PINNED" spends
      // half the line repeating itself, and which channel it settled on is the
      // thing you actually want when a board has gone quiet.
      if (s.recovery_channel > 0) {
        snprintf(rec.right, sizeof(rec.right), "CH%u", (unsigned)s.recovery_channel);
      } else if (s.recovery_failed) {
        snprintf(rec.right, sizeof(rec.right), "FAILED");
      } else {
        snprintf(rec.right, sizeof(rec.right), "SWEEPING");
      }
    }
  }

  // The footer has a whole line, so this is where the seconds go: a second
  // hand ticking once a second is the clearest evidence a panel is alive and
  // not a frozen frame, and there is no room for it beside the badges.
  UiFooterSlot& clock = slots[count++];
  if (s.time_current) {
    const time_t seconds = (time_t)(s.unix_ms / 1000ULL);
    struct tm utc {};
    if (gmtime_r(&seconds, &utc) != nullptr) {
      snprintf(clock.left, sizeof(clock.left), "UTC %02d:%02d:%02d",
               utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
      snprintf(clock.left, sizeof(clock.left), "UTC --:--:--");
    }
    snprintf(clock.right, sizeof(clock.right), "%s S%u",
             s.time_source && s.time_source[0] ? s.time_source : "?",
             (unsigned)s.stratum);
  } else if (s.time_known) {
    // Known but restored: say which, rather than showing the number.
    snprintf(clock.left, sizeof(clock.left), "UTC STALE");
    snprintf(clock.right, sizeof(clock.right), "FROM DISK");
  } else {
    snprintf(clock.left, sizeof(clock.left), "UTC");
    snprintf(clock.right, sizeof(clock.right), "UNKNOWN");
  }
  return count;
}

inline void ui_footer_draw_slot(GFXcanvas1& canvas, int y, const UiFooterSlot& slot,
                                bool inverted) {
  canvas.setTextColor(inverted ? 0 : 1);
  canvas.setCursor(UI_X_LEFT, y);
  canvas.print(slot.left);
  const int width = (int)strlen(slot.right) * UI_COL;
  canvas.setCursor(UI_X_RIGHT_EDGE - width, y);
  canvas.print(slot.right);
}

inline void ui_draw_footer_band(Adafruit_SSD1306& d, const NodeStatusView& s) {
  static GFXcanvas1 canvas(128, UI_FOOT_BAND_H);
  UiFooterSlot slots[UI_FOOT_SLOTS];
  const uint8_t count = ui_footer_build(s, slots);
  UiFooterState& st = ui_footer_state();
  if (st.current >= count) st.current = 0;
  if (st.next >= count) st.next = 0;

  const uint32_t now = millis();
  if (st.held_since == 0) st.held_since = now;

  if (st.roll < 0) {
    if (count > 1 && (now - st.held_since) >= UI_FOOT_HOLD_MS) {
      st.roll = 0;
      st.next = (uint8_t)((st.current + 1) % count);
    }
  } else {
    // One pixel per rendered frame: the smoothest step the panel has.
    st.roll++;
    if (st.roll >= UI_FOOT_BAND_H) {
      st.current = st.next;
      st.roll = -1;
      st.held_since = now;
    }
  }

  // The alarm is the background and does not rotate with the content.
  const bool trouble = !s.mesh_on || !s.interfaces_ok;
  canvas.fillScreen(trouble ? 1 : 0);

  // Text sits two pixels below the band top when at rest, which is where the
  // static footer used to be.
  const int rest = UI_Y_FOOT - UI_FOOT_BAND_Y;
  if (st.roll < 0) {
    ui_footer_draw_slot(canvas, rest, slots[st.current], trouble);
  } else {
    ui_footer_draw_slot(canvas, rest - st.roll, slots[st.current], trouble);
    ui_footer_draw_slot(canvas, rest - st.roll + UI_FOOT_BAND_H, slots[st.next], trouble);
  }

  // Blitting the canvas is what clips the rolling text to the band; drawing
  // straight onto the panel would smear it up through the body rows.
  d.drawBitmap(0, UI_FOOT_BAND_Y, canvas.getBuffer(), 128, UI_FOOT_BAND_H,
               SSD1306_WHITE, SSD1306_BLACK);
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

  // The clock, but only when there is one.
  //
  // A restored clock shown as "OLD 18:51" is worse than no clock: it puts a
  // plausible time on the glass and labels the lie in three characters that
  // are easy to stop seeing. When the time is not current this shows uptime
  // instead, which is true, useful, and cannot be mistaken for UTC.
  //
  // Ten characters fit beside the badges, so a protocol name and seconds
  // cannot both go here -- "NTP 18:51:22z" is thirteen. The protocol name
  // wins, because knowing a clock came from NTP rather than a relayed beacon
  // is worth more at a glance than its second hand, and the seconds are on the
  // footer's UTC slot where a whole line is free. Liveliness comes from the
  // colon instead: it blinks at 1Hz exactly as every digital clock has since
  // they were invented, which says "this is ticking" without costing a pixel.
  if (s.time_current) {
    const time_t seconds = (time_t)(s.unix_ms / 1000ULL);
    struct tm utc {};
    const bool tick = ((millis() / 500UL) % 2UL) == 0UL;
    if (gmtime_r(&seconds, &utc) != nullptr) {
      snprintf(buf, sizeof(buf), "%s %02d%c%02dz",
               s.time_source && s.time_source[0] ? s.time_source : "UTC",
               utc.tm_hour, tick ? ':' : ' ', utc.tm_min);
    } else {
      snprintf(buf, sizeof(buf), "--:--z");
    }
  } else {
    char uptime_text[10];
    ui_format_uptime(uptime_text, sizeof(uptime_text), s.uptime_s);
    snprintf(buf, sizeof(buf), "UP %s", uptime_text);
  }
  ui_right_text(d, UI_X_RIGHT_EDGE, UI_Y_IFACE, buf);

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
  ui_draw_footer_band(d, s);
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
