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

// A batch of telemetry reports a board could not deliver live (T2, LXMF reach,
// TAKDeliveryPlan.md). With no gateway reachable, the board keeps its reports,
// and about once an hour sends them as one LXMF message to the gateway through
// its own propagation store; the gateway collects it when it next reaches that
// store and writes each report at the time it was taken. Pure: no Arduino,
// radio or Reticulum, nothing allocated. Pinned against
// tools/telemetry_batch_codec.py by tests/fixtures/telemetry_batch_v1.json.
//
// Big-endian. Entries carry their report untouched -- a health report (0x01,
// TelemetryCodec.h) or a detail report (0x21, TelemetryDetailCodec.h) -- so
// the gateway decodes them with the codecs it already has.
//
//   off  size  field
//   0    1     kind_version   TELEMETRY_BATCH_WIRE_VERSION (0x31)
//   1    1     flags          BATCH_FLAG_*
//   2    4     sender_id      u32  as in the reports
//   6    4     composed_unix  u32  when the batch was composed; 0 = clock not set
//   10   1     count          u8   entries that follow, oldest first
//   then count x:
//        1     time_kind      BATCH_TIME_ABSOLUTE (unix seconds) or
//                             BATCH_TIME_RELATIVE (seconds before composed)
//        4     time           u32
//        2     len            u16  1 .. TELEMETRY_BATCH_ENTRY_MAX
//        len   report
//
// A board that kept more than fits drops the oldest and sets
// BATCH_FLAG_TRUNCATED: the newest reports matter most, and a gap at the start
// of a partition reads honestly on a dashboard.

#include <cstddef>
#include <cstdint>

#define TELEMETRY_BATCH_WIRE_VERSION 0x31
#define TELEMETRY_BATCH_MAX_LEN      2048   // about an hour of health and detail reports
#define TELEMETRY_BATCH_HEADER_LEN   11
#define TELEMETRY_BATCH_ENTRY_HEADER 7
#define TELEMETRY_BATCH_ENTRY_MAX    380    // the largest report, a full detail report
#define TELEMETRY_BATCH_MAX_ENTRIES  255

#define BATCH_FLAG_TRUNCATED 0x01   // older entries were dropped to fit

#define BATCH_TIME_RELATIVE 0x00    // seconds before composed (the clock was not set)
#define BATCH_TIME_ABSOLUTE 0x01    // unix seconds

struct BatchEntry {
  uint8_t time_kind = BATCH_TIME_RELATIVE;
  uint32_t time = 0;
  uint16_t len = 0;
  const uint8_t* report = nullptr;
};

struct BatchHeader {
  uint8_t flags = 0;
  uint32_t sender_id = 0;
  uint32_t composed_unix = 0;
  uint8_t count = 0;
};

inline void batch_put32(uint8_t* out, size_t& at, uint32_t v) {
  out[at++] = (uint8_t)(v >> 24); out[at++] = (uint8_t)(v >> 16);
  out[at++] = (uint8_t)(v >> 8);  out[at++] = (uint8_t)v;
}
inline uint32_t batch_get32(const uint8_t* in) {
  return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

inline bool batch_entry_valid(const BatchEntry& e) {
  return e.report != nullptr && e.len > 0 && e.len <= TELEMETRY_BATCH_ENTRY_MAX &&
         (e.time_kind == BATCH_TIME_RELATIVE || e.time_kind == BATCH_TIME_ABSOLUTE);
}

// Encodes `entries` (oldest first) into `out`. When they do not all fit, the
// oldest are left out and BATCH_FLAG_TRUNCATED is set, as it is when the board
// says it already dropped some (`truncated`). Returns the length, or 0 when an
// entry is invalid or not even the header fits.
inline size_t telemetry_batch_encode(uint32_t sender_id, uint32_t composed_unix, bool truncated,
                                     const BatchEntry* entries, size_t n,
                                     uint8_t* out, size_t out_len) {
  const size_t limit = out_len < TELEMETRY_BATCH_MAX_LEN ? out_len : TELEMETRY_BATCH_MAX_LEN;
  if (out == nullptr || limit < TELEMETRY_BATCH_HEADER_LEN) return 0;
  for (size_t i = 0; i < n; ++i) if (!batch_entry_valid(entries[i])) return 0;

  // The newest that fit, counting back from the end.
  size_t first = n;
  size_t total = TELEMETRY_BATCH_HEADER_LEN;
  while (first > 0 && (n - first) < TELEMETRY_BATCH_MAX_ENTRIES) {
    const size_t need = TELEMETRY_BATCH_ENTRY_HEADER + entries[first - 1].len;
    if (total + need > limit) break;
    total += need;
    --first;
  }
  uint8_t flags = 0;
  if (truncated || first > 0) flags |= BATCH_FLAG_TRUNCATED;

  size_t at = 0;
  out[at++] = TELEMETRY_BATCH_WIRE_VERSION;
  out[at++] = flags;
  batch_put32(out, at, sender_id);
  batch_put32(out, at, composed_unix);
  out[at++] = (uint8_t)(n - first);
  for (size_t i = first; i < n; ++i) {
    const BatchEntry& e = entries[i];
    out[at++] = e.time_kind;
    batch_put32(out, at, e.time);
    out[at++] = (uint8_t)(e.len >> 8);
    out[at++] = (uint8_t)e.len;
    for (uint16_t k = 0; k < e.len; ++k) out[at++] = e.report[k];
  }
  return at;
}

// Reads the header and checks the whole batch: every entry in bounds and
// valid, exactly `count` of them, nothing left over. Entries are then read in
// order with telemetry_batch_next(), their reports pointing into `in`.
inline bool telemetry_batch_header(const uint8_t* in, size_t len, BatchHeader& out) {
  if (in == nullptr || len < TELEMETRY_BATCH_HEADER_LEN || len > TELEMETRY_BATCH_MAX_LEN) return false;
  if (in[0] != TELEMETRY_BATCH_WIRE_VERSION) return false;
  BatchHeader h;
  h.flags = in[1];
  h.sender_id = batch_get32(in + 2);
  h.composed_unix = batch_get32(in + 6);
  h.count = in[10];
  size_t at = TELEMETRY_BATCH_HEADER_LEN;
  for (uint8_t i = 0; i < h.count; ++i) {
    if (at + TELEMETRY_BATCH_ENTRY_HEADER > len) return false;
    const uint8_t kind = in[at];
    const uint16_t elen = (uint16_t)(((uint16_t)in[at + 5] << 8) | in[at + 6]);
    if (kind != BATCH_TIME_RELATIVE && kind != BATCH_TIME_ABSOLUTE) return false;
    if (elen == 0 || elen > TELEMETRY_BATCH_ENTRY_MAX) return false;
    at += TELEMETRY_BATCH_ENTRY_HEADER + elen;
    if (at > len) return false;
  }
  if (at != len) return false;
  out = h;
  return true;
}

// The entry at `at` (start at TELEMETRY_BATCH_HEADER_LEN), advancing `at`.
// Only after telemetry_batch_header() accepted the batch.
inline void telemetry_batch_next(const uint8_t* in, size_t& at, BatchEntry& out) {
  out.time_kind = in[at];
  out.time = batch_get32(in + at + 1);
  out.len = (uint16_t)(((uint16_t)in[at + 5] << 8) | in[at + 6]);
  out.report = in + at + TELEMETRY_BATCH_ENTRY_HEADER;
  at += TELEMETRY_BATCH_ENTRY_HEADER + out.len;
}
