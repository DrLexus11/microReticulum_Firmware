// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// Where a board sends its health report, and when (PR F step F4a). Pure: no
// Arduino, no Reticulum, so it is tested on Linux; the service in
// LoopServicesImpl.h feeds it announces and path facts and does the sending.
//
// Any node with an uplink -- the deck, a Vox, a board on Wi-Fi -- is a gateway:
// it announces rnstransport.telemetry.uplink and publishes what it receives to
// MQTT (TAKDeliveryPlan.md, "PR F starts with mesh telemetry"). A board keeps
// the few gateways it has heard and sends to the nearest one it has a path to,
// so no single gateway is a point of failure. With none reachable it keeps
// trying, and the report it sends when one appears is current: every count in
// it is cumulative, so the latest report carries everything an older one did.

#include <cstddef>
#include <cstdint>
#include <cstring>

#define TELEMETRY_UPLINK_ASPECT "telemetry.uplink"
#define TELEMETRY_UPLINK_FILTER "rnstransport.telemetry.uplink"

#ifndef TELEMETRY_INTERVAL_MS
#define TELEMETRY_INTERVAL_MS 300000u     // one report per board per 5 minutes
#endif
#ifndef TELEMETRY_FIRST_MS
#define TELEMETRY_FIRST_MS 60000u         // first report a minute after boot
#endif
#ifndef TELEMETRY_RETRY_MS
#define TELEMETRY_RETRY_MS 30000u         // while no gateway can be reached
#endif
#ifndef TELEMETRY_GATEWAY_EXPIRY_MS
#define TELEMETRY_GATEWAY_EXPIRY_MS 7200000u  // a gateway unheard for 2 h is forgotten
#endif
#ifndef TELEMETRY_GATEWAY_CAPACITY
#define TELEMETRY_GATEWAY_CAPACITY 4
#endif

#define TELEMETRY_HASH_LEN 16
#define TELEMETRY_HOPS_UNKNOWN 0xFF

// The gateways this board has heard announce. Fixed size, no allocation: when
// full, the one heard longest ago makes room.
class TelemetryGateways {
public:
  struct Entry {
    uint8_t hash[TELEMETRY_HASH_LEN];
    uint32_t heard_ms;
    bool used;
  };

  // An announce from a gateway. Returns false only for a malformed hash.
  bool heard(const uint8_t* hash, size_t len, uint32_t now_ms) {
    if (hash == nullptr || len != TELEMETRY_HASH_LEN) return false;
    Entry* slot = find(hash);
    if (slot == nullptr) slot = free_or_oldest(now_ms);
    memcpy(slot->hash, hash, TELEMETRY_HASH_LEN);
    slot->heard_ms = now_ms;
    slot->used = true;
    return true;
  }

  // The gateway to send to: of those heard within the expiry, the one with a
  // path and the fewest hops, the most recently heard breaking a tie. Null
  // when none has a path. `hops` returns TELEMETRY_HOPS_UNKNOWN for no path.
  template <typename HopsFn>
  // `max_age_ms` narrows it to gateways heard recently. A path outlives the
  // gateway it leads to by days, so a board that keeps its reports when out
  // of reach (T2) asks for one heard within a few announce intervals.
  const Entry* best(uint32_t now_ms, HopsFn hops, uint32_t max_age_ms = TELEMETRY_GATEWAY_EXPIRY_MS) const {
    const Entry* chosen = nullptr;
    uint8_t chosen_hops = TELEMETRY_HOPS_UNKNOWN;
    for (const Entry& e : _entries) {
      if (!current(e, now_ms) || now_ms - e.heard_ms >= max_age_ms) continue;
      const uint8_t h = hops(e.hash);
      if (h == TELEMETRY_HOPS_UNKNOWN) continue;
      if (chosen == nullptr || h < chosen_hops ||
          (h == chosen_hops && (int32_t)(e.heard_ms - chosen->heard_ms) > 0)) {
        chosen = &e;
        chosen_hops = h;
      }
    }
    return chosen;
  }

  // The most recently heard gateway, path or not: the one to ask a path for.
  const Entry* newest(uint32_t now_ms) const {
    const Entry* chosen = nullptr;
    for (const Entry& e : _entries) {
      if (!current(e, now_ms)) continue;
      if (chosen == nullptr || (int32_t)(e.heard_ms - chosen->heard_ms) > 0) chosen = &e;
    }
    return chosen;
  }

  size_t count(uint32_t now_ms) const {
    size_t n = 0;
    for (const Entry& e : _entries) if (current(e, now_ms)) ++n;
    return n;
  }

private:
  static bool current(const Entry& e, uint32_t now_ms) {
    return e.used && now_ms - e.heard_ms < TELEMETRY_GATEWAY_EXPIRY_MS;
  }

  Entry* find(const uint8_t* hash) {
    for (Entry& e : _entries) {
      if (e.used && memcmp(e.hash, hash, TELEMETRY_HASH_LEN) == 0) return &e;
    }
    return nullptr;
  }

  Entry* free_or_oldest(uint32_t now_ms) {
    Entry* oldest = &_entries[0];
    for (Entry& e : _entries) {
      if (!e.used) return &e;
      if (now_ms - e.heard_ms > now_ms - oldest->heard_ms) oldest = &e;
    }
    return oldest;
  }

  Entry _entries[TELEMETRY_GATEWAY_CAPACITY] = {};
};

// When the next report is due. A sent report waits the interval; a report that
// found no gateway is retried sooner, so a gateway coming into range hears this
// board within TELEMETRY_RETRY_MS rather than a whole interval later.
class TelemetrySchedule {
public:
  explicit TelemetrySchedule(uint32_t boot_ms) : _next(boot_ms + TELEMETRY_FIRST_MS) {}

  bool due(uint32_t now_ms) const { return (int32_t)(now_ms - _next) >= 0; }
  void sent(uint32_t now_ms) { _next = now_ms + TELEMETRY_INTERVAL_MS; _last_sent = now_ms; _ever_sent = true; }
  void unreachable(uint32_t now_ms) { _next = now_ms + TELEMETRY_RETRY_MS; }

  bool ever_sent() const { return _ever_sent; }
  uint32_t last_sent() const { return _last_sent; }

private:
  uint32_t _next;
  uint32_t _last_sent = 0;
  bool _ever_sent = false;
};
