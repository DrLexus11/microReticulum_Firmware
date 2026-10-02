// Copyright (C) 2026, Chad Attermann

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

// The compact position report: its fix type and its wire format, with no
// Arduino, radio or Reticulum dependency (PR F step F2, R2 of IoTPlatform.md).
// Moved verbatim from Position.h (the types) and PositionReport.h (the wire
// format); those files include this one. Host-tested in
// test/test_firmware_core, and its bytes pinned against the deck's Python twin,
// tools/position_codec.py, by tests/fixtures/position_v2.json.

#include <cstddef>
#include <cstdint>

enum class NodePositionKind : uint8_t {
  NONE  = 0,
  PHONE = 1,   // Columba, over the mesh
  GNSS  = 2,   // a receiver wired to this board
  FIXED = 3,   // surveyed once and stored: a mast, a base station
};

// Scaled integers rather than floats, because this struct becomes a wire format
// in step 2 and a float on the wire is a portability question nobody needs.
// 1e-7 degrees is about 1.1 cm at the equator, well past what any of these
// receivers can actually resolve.
struct NodePositionFix {
  bool valid = false;

  int32_t lat_e7 = 0;          // degrees x 1e7, positive north
  int32_t lon_e7 = 0;          // degrees x 1e7, positive east

  // Altitude is separately flagged because zero is a real altitude and a
  // receiver without an altitude solution must not be read as reporting sea
  // level. The same reasoning as reporting an unknown clock as unknown rather
  // than as the epoch.
  bool alt_known = false;
  int16_t alt_m = 0;           // metres, height above ellipsoid

  // Zero means "not reported". A CoT event without a circular error is
  // legitimate; one claiming zero error is not.
  uint16_t accuracy_m = 0;

  bool course_known = false;
  uint16_t course_ddeg = 0;    // tenths of a degree, 0..3599
  uint16_t speed_cms = 0;      // cm/s

  uint8_t sats = 0;            // 0 when the source does not report it

  // Who this fix is about. Four bytes of the reporting node's identity hash,
  // zero when unknown.
  //
  // It has to travel with the fix because nothing else carries it: a Reticulum
  // packet to a SINGLE destination is anonymous by construction, so a receiver
  // has no way to tell two senders apart. Without this every report is a new
  // track, and a map fills with one person's ghosts -- which is exactly what
  // the first two live reports did on 2026-09-06.
  //
  // An identifier, not an authentication. These packets are unsigned, so this
  // says which track a report belongs to and nothing about who wrote it.
  uint32_t sender_id = 0;

  // When the fix was taken, by the source's own clock. Zero means the source
  // had no clock -- which is a real case for a bare GNSS module before its
  // first time solution, and for this node generally. Never substitute our
  // clock here: a fix stamped with the time we *received* it is a different
  // measurement wearing the same field.
  uint64_t fix_unix_ms = 0;

  // Always set, from millis() at the moment the fix was accepted. This is what
  // ages a fix, because it works whether or not anybody involved knows the
  // wall time -- and a node with no clock still must not report a stale
  // position as current.
  uint32_t received_ms = 0;

  NodePositionKind kind = NodePositionKind::NONE;
};

// --- wire format -------------------------------------------------------------
//
// A fixed big-endian layout rather than msgpack. Everywhere else in this
// firmware msgpack is the right answer, because the payloads are structured and
// the cost of a few key bytes is irrelevant. Here it is the whole point: a
// msgpack map of these fields runs to forty or fifty bytes against eighteen,
// and §2's budget is the reason this feature is shaped the way it is at all.
// TimeBeacon.h signs a fixed layout for the same reason.
//
//   off  size  field
//   0    1     version
//   1    1     flags
//   2    4     sender_id    uint32  four bytes of the sender's identity hash
//   6    4     lat_e7       int32   degrees x 1e7, positive north
//   10   4     lon_e7       int32   degrees x 1e7, positive east
//   14   4     fix_unix_s   uint32  seconds; 0 when the source had no clock
//   18   1     accuracy_m   uint8   0 unreported, 1..254 metres, 255 = over
//   -- then, in flag order, only what is present --
//   +2         alt_m        int16   metres HAE          FLAG_ALT
//   +1         course       uint8   2-degree units      FLAG_COURSE
//   +1         speed        uint8   half-metre/s units  FLAG_SPEED
//   +1         sats         uint8                       FLAG_SATS
//   +1         interval     uint8   minutes to the next FLAG_INTERVAL
//
// Nineteen bytes minimum, twenty-five full. Against ~700 for the XML that comes
// out of the gateway at the other end.
//
// Version 2 added sender_id, and version 1 is refused rather than accepted
// without one. A Reticulum packet to a SINGLE destination is anonymous by
// construction, so a v1 report cannot be attributed to anybody -- and a
// receiver that accepted it would put every report on its own track. Measured
// on 2026-09-06: two reports from one phone arrived as two separate tracks.
// Nothing is deployed on v1 outside this bench, so there is nothing to be
// compatible with and a silent downgrade would only hide the bug.
#define POSITION_WIRE_VERSION 2
#define POSITION_WIRE_BASE_LEN 19
#define POSITION_WIRE_MAX_LEN 25

#define POSITION_FLAG_ALT    0x01
#define POSITION_FLAG_COURSE 0x02
#define POSITION_FLAG_SPEED  0x04
#define POSITION_FLAG_SATS   0x08
// The sender's own statement of when it reports next, so a receiver keeps the
// track current that long. Set by a handset reporting while ATAK is closed;
// this node never sets it, and the decoder below stops at the fields it reads,
// so the trailing byte is ignored rather than refused.
#define POSITION_FLAG_INTERVAL 0x10

// Seconds, not milliseconds. CoT staleness is a minute-scale concern and four
// bytes of seconds reach 2106; milliseconds would cost four more bytes to say
// nothing anyone downstream can use.
inline size_t position_report_encode(const NodePositionFix& fix,
                                     uint8_t* out, size_t out_len) {
  if (!fix.valid || out == nullptr || out_len < POSITION_WIRE_BASE_LEN) return 0;

  uint8_t flags = 0;
  if (fix.alt_known)    flags |= POSITION_FLAG_ALT;
  if (fix.course_known) flags |= POSITION_FLAG_COURSE;
  if (fix.speed_cms > 0) flags |= POSITION_FLAG_SPEED;
  if (fix.sats > 0)     flags |= POSITION_FLAG_SATS;

  const uint32_t lat = (uint32_t)fix.lat_e7;
  const uint32_t lon = (uint32_t)fix.lon_e7;
  const uint32_t secs = (uint32_t)(fix.fix_unix_ms / 1000ULL);
  const uint32_t sender = fix.sender_id;

  size_t at = 0;
  out[at++] = POSITION_WIRE_VERSION;
  out[at++] = flags;
  out[at++] = (uint8_t)(sender >> 24); out[at++] = (uint8_t)(sender >> 16);
  out[at++] = (uint8_t)(sender >> 8);  out[at++] = (uint8_t)(sender);
  out[at++] = (uint8_t)(lat >> 24); out[at++] = (uint8_t)(lat >> 16);
  out[at++] = (uint8_t)(lat >> 8);  out[at++] = (uint8_t)(lat);
  out[at++] = (uint8_t)(lon >> 24); out[at++] = (uint8_t)(lon >> 16);
  out[at++] = (uint8_t)(lon >> 8);  out[at++] = (uint8_t)(lon);
  out[at++] = (uint8_t)(secs >> 24); out[at++] = (uint8_t)(secs >> 16);
  out[at++] = (uint8_t)(secs >> 8);  out[at++] = (uint8_t)(secs);
  // Saturate rather than wrap. A receiver reporting 400 m of error that
  // arrives as 144 is a marker the operator will trust far more than it
  // deserves.
  out[at++] = (fix.accuracy_m == 0) ? 0
            : (fix.accuracy_m > 254 ? 255 : (uint8_t)fix.accuracy_m);

  if (flags & POSITION_FLAG_ALT) {
    if (at + 2 > out_len) return 0;
    const uint16_t alt = (uint16_t)fix.alt_m;
    out[at++] = (uint8_t)(alt >> 8); out[at++] = (uint8_t)(alt);
  }
  if (flags & POSITION_FLAG_COURSE) {
    if (at + 1 > out_len) return 0;
    // Normalise before scaling. A source handing over a full turn -- 3600
    // tenths, which is due north -- would otherwise scale to 180 and decode
    // as due south, and a heading is exactly the field where a silent
    // 180-degree error is unrecoverable by the person reading the map.
    out[at++] = (uint8_t)((fix.course_ddeg % 3600) / 20);   // 0..179
  }
  if (flags & POSITION_FLAG_SPEED) {
    if (at + 1 > out_len) return 0;
    const uint32_t units = fix.speed_cms / 50;     // half a metre per second
    out[at++] = (units > 255) ? 255 : (uint8_t)units;
  }
  if (flags & POSITION_FLAG_SATS) {
    if (at + 1 > out_len) return 0;
    out[at++] = fix.sats;
  }
  return at;
}

// The inverse, for the gateway, for a node that relays, and for tests. A
// decoder that cannot round-trip its own encoder is the classic way a wire
// format goes wrong in the field rather than on the bench.
inline bool position_report_decode(const uint8_t* in, size_t len,
                                   NodePositionFix& out) {
  if (in == nullptr || len < POSITION_WIRE_BASE_LEN) return false;
  if (in[0] != POSITION_WIRE_VERSION) return false;

  const uint8_t flags = in[1];
  out = NodePositionFix{};
  out.sender_id = ((uint32_t)in[2] << 24) | ((uint32_t)in[3] << 16) |
                  ((uint32_t)in[4] << 8) | (uint32_t)in[5];
  out.lat_e7 = (int32_t)(((uint32_t)in[6] << 24) | ((uint32_t)in[7] << 16) |
                         ((uint32_t)in[8] << 8) | (uint32_t)in[9]);
  out.lon_e7 = (int32_t)(((uint32_t)in[10] << 24) | ((uint32_t)in[11] << 16) |
                         ((uint32_t)in[12] << 8) | (uint32_t)in[13]);
  const uint32_t secs = ((uint32_t)in[14] << 24) | ((uint32_t)in[15] << 16) |
                        ((uint32_t)in[16] << 8) | (uint32_t)in[17];
  out.fix_unix_ms = (uint64_t)secs * 1000ULL;
  out.accuracy_m = in[18];

  size_t at = POSITION_WIRE_BASE_LEN;
  if (flags & POSITION_FLAG_ALT) {
    if (at + 2 > len) return false;
    out.alt_known = true;
    out.alt_m = (int16_t)(((uint16_t)in[at] << 8) | (uint16_t)in[at + 1]);
    at += 2;
  }
  if (flags & POSITION_FLAG_COURSE) {
    if (at + 1 > len) return false;
    out.course_known = true;
    out.course_ddeg = (uint16_t)in[at++] * 20;
  }
  if (flags & POSITION_FLAG_SPEED) {
    if (at + 1 > len) return false;
    out.speed_cms = (uint16_t)in[at++] * 50;
  }
  if (flags & POSITION_FLAG_SATS) {
    if (at + 1 > len) return false;
    out.sats = in[at++];
  }
  out.valid = true;
  return true;
}

