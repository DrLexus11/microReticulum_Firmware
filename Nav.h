// Two buttons, one page cursor.
//
// Input.h already exists and is upstream GPL code shared with every board that
// has a user button. It handles exactly one, with click/double/triple
// detection, and is wired into the .ino through HAS_INPUT together with a
// quick-reset console behaviour that has nothing to do with a display. Bending
// it to two buttons would fork upstream code and change boards this work never
// touches, so this sits beside it instead: press-to-advance, nothing else.
//
// The panel gets four pages and the design gives it two buttons, so a press is
// unambiguous -- there is no long-press vocabulary to learn and nothing to
// discover. The Rev 2 mezzanine has four buttons; when it arrives the extra two
// become direct page selects, and this stays the fallback for two-button
// hardware.
#pragma once

#include <Arduino.h>
#include "Boards.h"

#if defined(HAS_NAV_BUTTONS) && HAS_NAV_BUTTONS == true

#ifndef NAV_DEBOUNCE_MS
#define NAV_DEBOUNCE_MS 25
#endif

// Pages defined by docs/ui/ssd1306. Page 1 in the design is index 0 here.
enum NavPage : uint8_t {
  NAV_PAGE_MAIN = 0,
  NAV_PAGE_RRC = 1,
  NAV_PAGE_PEERS = 2,
  NAV_PAGE_INTERFACES = 3,
  NAV_PAGE_COUNT = 4,
};

struct NavButton {
  uint8_t pin;
  int state;
  int debounce_state;
  uint32_t debounce_last;
};

inline NavButton nav_prev_button = {(uint8_t)pin_nav_prev, HIGH, HIGH, 0};
inline NavButton nav_next_button = {(uint8_t)pin_nav_next, HIGH, HIGH, 0};
inline uint8_t nav_page_index = NAV_PAGE_MAIN;
inline uint32_t nav_last_activity = 0;

inline void nav_init() {
  pinMode(nav_prev_button.pin, INPUT_PULLUP);
  pinMode(nav_next_button.pin, INPUT_PULLUP);
}

// Returns true on the press edge, having debounced. Released is HIGH because
// the buttons pull to ground against the internal pull-up -- a breadboard
// button needs no external parts that way.
inline bool nav_button_pressed(NavButton& button) {
  const int reading = digitalRead(button.pin);
  if (reading != button.debounce_state) {
    button.debounce_last = millis();
    button.debounce_state = reading;
  }
  if ((millis() - button.debounce_last) <= NAV_DEBOUNCE_MS) return false;
  if (reading == button.state) return false;
  button.state = reading;
  return (reading == LOW);
}

inline uint8_t nav_page() { return nav_page_index; }

inline void nav_read() {
  if (nav_button_pressed(nav_prev_button)) {
    nav_page_index = (uint8_t)((nav_page_index + NAV_PAGE_COUNT - 1) % NAV_PAGE_COUNT);
    nav_last_activity = millis();
  }
  if (nav_button_pressed(nav_next_button)) {
    nav_page_index = (uint8_t)((nav_page_index + 1) % NAV_PAGE_COUNT);
    nav_last_activity = millis();
  }
}

#else

inline void nav_init() {}
inline void nav_read() {}
inline uint8_t nav_page() { return 0; }

#endif
