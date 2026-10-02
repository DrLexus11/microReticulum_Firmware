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

// A board's health report: the mesh telemetry of PR F (TAKDeliveryPlan.md, "PR
// F starts with mesh telemetry"). Each board sends one to a gateway at a
// budgeted interval; the gateway publishes it to MQTT, and the plugin's
// mesh-health view reads the same data. Pure, like PositionCodec.h: no Arduino,
// radio or Reticulum, host-tested in test/test_firmware_core and pinned against
// the deck's Python twin, tools/telemetry_codec.py, by
// tests/fixtures/telemetry_v1.json.
//
// A fixed big-endian layout for the reason the position report has one: on LoRa
// every byte is shared airtime, and msgpack keys would double the size.
//
//   off  size  field
//   0    1     version            TELEMETRY_WIRE_VERSION
//   1    1     flags              TELEMETRY_FLAG_*
//   2    4     sender_id   u32    four bytes of the sender's identity hash
//   6    4     uptime_s    u32
//   10   1     reset       u8     TELEMETRY_RESET_*: why the current run began
//   11   2     boots       u16    since power was last applied
//   13   2     crashes     u16    lifetime: watchdog and brownout restarts
//   15   2     panics      u16    lifetime: exception restarts
//   17   2     heap_kb     u16    malloc-usable internal heap (MALLOC_CAP_8BIT)
//   19   2     largest_kb  u16    its largest free block
//   21   1     if_present  u8     TELEMETRY_IF_*: compiled in and initialised
//   22   1     if_up       u8     TELEMETRY_IF_*: up and usable now
//   23   1     ble_peers   u8
//   24   1     espnow_peers u8
//   25   2     paths       u16    path-table entries
//   27   2     nodes       u16    distinct identities heard
//   -- then, in flag order, only what is present --
//   +2         psram_kb    u16    FLAG_PSRAM
//   +2         battery_mv  u16    FLAG_BATTERY
//   +1         battery_pct u8     FLAG_BATTERY
//
// Twenty-nine bytes minimum, thirty-four full. Counts saturate rather than
// wrap -- a board that has crashed 70000 times must not report 4464 -- and
// heap figures are whole kilobytes, rounded down.
//
// heap_kb is the malloc-usable figure, not MALLOC_CAP_INTERNAL: on an ESP32
// without PSRAM the latter also counts instruction RAM that only takes 32-bit
// access, and overstated the OZD's free memory by about 13 KB (2026-10-01).

#include <cstddef>
#include <cstdint>

#define TELEMETRY_WIRE_VERSION  1
#define TELEMETRY_WIRE_BASE_LEN 29
#define TELEMETRY_WIRE_MAX_LEN  34

#define TELEMETRY_FLAG_RELAYING       0x01  // forwarding for others
#define TELEMETRY_FLAG_RELAY_EXPECTED 0x02  // two or more carriers up
#define TELEMETRY_FLAG_TIME_CURRENT   0x04  // clock adopted from a live source
#define TELEMETRY_FLAG_PSRAM          0x08  // psram_kb follows
#define TELEMETRY_FLAG_BATTERY        0x10  // battery_mv and battery_pct follow

#define TELEMETRY_IF_LORA   0x01
#define TELEMETRY_IF_BLE    0x02
#define TELEMETRY_IF_WIFI   0x04
#define TELEMETRY_IF_ESPNOW 0x08
#define TELEMETRY_IF_HALOW  0x10

// Why the current run began. Codes, not the reset-reason strings the firmware
// logs, so the report stays fixed-size; the gateway names them.
#define TELEMETRY_RESET_UNKNOWN  0
#define TELEMETRY_RESET_POWERON  1
#define TELEMETRY_RESET_SOFTWARE 2   // ESP.restart(), RNS_LOW_MEMORY_REBOOT
#define TELEMETRY_RESET_PANIC    3   // exception, abort()
#define TELEMETRY_RESET_TASK_WDT 4
#define TELEMETRY_RESET_INT_WDT  5
#define TELEMETRY_RESET_BROWNOUT 6
#define TELEMETRY_RESET_EXTERNAL 7   // EN pin
#define TELEMETRY_RESET_OTHER    8

struct NodeTelemetry {
  uint32_t sender_id = 0;
  uint32_t uptime_s = 0;
  uint8_t reset = TELEMETRY_RESET_UNKNOWN;
  uint32_t boots = 0;
  uint32_t crashes = 0;
  uint32_t panics = 0;
  uint32_t heap_bytes = 0;      // encoded as whole KB
  uint32_t largest_bytes = 0;   // encoded as whole KB
  bool psram_known = false;
  uint32_t psram_bytes = 0;     // encoded as whole KB
  uint8_t if_present = 0;
  uint8_t if_up = 0;
  uint16_t ble_peers = 0;
  uint16_t espnow_peers = 0;
  uint32_t paths = 0;
  uint32_t nodes = 0;
  bool relaying = false;
  bool relay_expected = false;
  bool time_current = false;
  bool battery_known = false;
  uint16_t battery_mv = 0;
  uint8_t battery_pct = 0;
};

inline uint16_t telemetry_sat16(uint32_t value) {
  return value > 0xFFFFu ? 0xFFFFu : (uint16_t)value;
}

inline uint8_t telemetry_sat8(uint32_t value) {
  return value > 0xFFu ? 0xFFu : (uint8_t)value;
}

inline void telemetry_put16(uint8_t* out, size_t& at, uint16_t value) {
  out[at++] = (uint8_t)(value >> 8);
  out[at++] = (uint8_t)value;
}

inline uint16_t telemetry_get16(const uint8_t* in) {
  return (uint16_t)(((uint16_t)in[0] << 8) | in[1]);
}

// The report's bytes, or 0 when `out` is too small.
inline size_t telemetry_encode(const NodeTelemetry& t, uint8_t* out, size_t out_len) {
  if (out == nullptr || out_len < TELEMETRY_WIRE_BASE_LEN) return 0;

  uint8_t flags = 0;
  if (t.relaying)       flags |= TELEMETRY_FLAG_RELAYING;
  if (t.relay_expected) flags |= TELEMETRY_FLAG_RELAY_EXPECTED;
  if (t.time_current)   flags |= TELEMETRY_FLAG_TIME_CURRENT;
  if (t.psram_known)    flags |= TELEMETRY_FLAG_PSRAM;
  if (t.battery_known)  flags |= TELEMETRY_FLAG_BATTERY;

  size_t at = 0;
  out[at++] = TELEMETRY_WIRE_VERSION;
  out[at++] = flags;
  out[at++] = (uint8_t)(t.sender_id >> 24); out[at++] = (uint8_t)(t.sender_id >> 16);
  out[at++] = (uint8_t)(t.sender_id >> 8);  out[at++] = (uint8_t)t.sender_id;
  out[at++] = (uint8_t)(t.uptime_s >> 24);  out[at++] = (uint8_t)(t.uptime_s >> 16);
  out[at++] = (uint8_t)(t.uptime_s >> 8);   out[at++] = (uint8_t)t.uptime_s;
  out[at++] = t.reset;
  telemetry_put16(out, at, telemetry_sat16(t.boots));
  telemetry_put16(out, at, telemetry_sat16(t.crashes));
  telemetry_put16(out, at, telemetry_sat16(t.panics));
  telemetry_put16(out, at, telemetry_sat16(t.heap_bytes / 1024));
  telemetry_put16(out, at, telemetry_sat16(t.largest_bytes / 1024));
  out[at++] = t.if_present;
  out[at++] = t.if_up;
  out[at++] = telemetry_sat8(t.ble_peers);
  out[at++] = telemetry_sat8(t.espnow_peers);
  telemetry_put16(out, at, telemetry_sat16(t.paths));
  telemetry_put16(out, at, telemetry_sat16(t.nodes));

  if (flags & TELEMETRY_FLAG_PSRAM) {
    if (at + 2 > out_len) return 0;
    telemetry_put16(out, at, telemetry_sat16(t.psram_bytes / 1024));
  }
  if (flags & TELEMETRY_FLAG_BATTERY) {
    if (at + 3 > out_len) return 0;
    telemetry_put16(out, at, t.battery_mv);
    out[at++] = t.battery_pct > 100 ? 100 : t.battery_pct;
  }
  return at;
}

// The inverse, for the gateway, a relaying node and the tests. Heap figures come
// back as whole KB times 1024. Bytes past the fields the flags announce are
// ignored, so a later version can append without breaking this reader.
inline bool telemetry_decode(const uint8_t* in, size_t len, NodeTelemetry& out) {
  if (in == nullptr || len < TELEMETRY_WIRE_BASE_LEN) return false;
  if (in[0] != TELEMETRY_WIRE_VERSION) return false;

  const uint8_t flags = in[1];
  out = NodeTelemetry{};
  out.sender_id = ((uint32_t)in[2] << 24) | ((uint32_t)in[3] << 16) |
                  ((uint32_t)in[4] << 8) | (uint32_t)in[5];
  out.uptime_s = ((uint32_t)in[6] << 24) | ((uint32_t)in[7] << 16) |
                 ((uint32_t)in[8] << 8) | (uint32_t)in[9];
  out.reset = in[10];
  out.boots = telemetry_get16(in + 11);
  out.crashes = telemetry_get16(in + 13);
  out.panics = telemetry_get16(in + 15);
  out.heap_bytes = (uint32_t)telemetry_get16(in + 17) * 1024u;
  out.largest_bytes = (uint32_t)telemetry_get16(in + 19) * 1024u;
  out.if_present = in[21];
  out.if_up = in[22];
  out.ble_peers = in[23];
  out.espnow_peers = in[24];
  out.paths = telemetry_get16(in + 25);
  out.nodes = telemetry_get16(in + 27);
  out.relaying = (flags & TELEMETRY_FLAG_RELAYING) != 0;
  out.relay_expected = (flags & TELEMETRY_FLAG_RELAY_EXPECTED) != 0;
  out.time_current = (flags & TELEMETRY_FLAG_TIME_CURRENT) != 0;

  size_t at = TELEMETRY_WIRE_BASE_LEN;
  if (flags & TELEMETRY_FLAG_PSRAM) {
    if (at + 2 > len) return false;
    out.psram_known = true;
    out.psram_bytes = (uint32_t)telemetry_get16(in + at) * 1024u;
    at += 2;
  }
  if (flags & TELEMETRY_FLAG_BATTERY) {
    if (at + 3 > len) return false;
    out.battery_known = true;
    out.battery_mv = telemetry_get16(in + at);
    out.battery_pct = in[at + 2];
    at += 3;
  }
  return true;
}
