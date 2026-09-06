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

// Getting a position off this node and to a gateway that speaks CoT.
//
// TAKCapability.md §7 step 2. Two decisions were already made there and are
// worth restating where the code is, because both look like premature
// optimisation until you have the airtime numbers:
//
// §2: raw CoT XML is ~700 bytes, which is 538 ms on air at SF7/BW250 and
// sixty-seven reports an hour channel-wide -- one person reporting once a
// minute while nobody else transmits anything, ever. It does not fit. Compact
// on the wire, expanded to CoT at the gateway, is the only shape that works,
// and it is what Meshtastic concluded too.
//
// §3: unicast to a fixed gateway, not broadcast. Reticulum is not a flooding
// network. Reaching a destination needs a path, paths are learned from
// announces, and announces are deliberately expensive -- so announcing every
// minute to carry twenty bytes would spend far more on routing than on the
// payload. The gateway is stationary and announces on a normal schedule, so
// every node keeps a current path *to it* even while its own reachability
// changes underneath it.

#include <Arduino.h>

#if defined(HAS_RNS)

#include <microReticulum/Transport.h>
#include <microReticulum/Destination.h>
#include <microReticulum/Packet.h>
#include "Position.h"

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
//   2    4     lat_e7       int32   degrees x 1e7, positive north
//   6    4     lon_e7       int32   degrees x 1e7, positive east
//   10   4     fix_unix_s   uint32  seconds; 0 when the source had no clock
//   14   1     accuracy_m   uint8   0 unreported, 1..254 metres, 255 = over
//   -- then, in flag order, only what is present --
//   +2         alt_m        int16   metres HAE          FLAG_ALT
//   +1         course       uint8   2-degree units      FLAG_COURSE
//   +1         speed        uint8   half-metre/s units  FLAG_SPEED
//   +1         sats         uint8                       FLAG_SATS
//
// Fifteen bytes minimum, twenty full. Against ~700 for the XML that comes out
// of the gateway at the other end.
#define POSITION_WIRE_VERSION 1
#define POSITION_WIRE_BASE_LEN 15
#define POSITION_WIRE_MAX_LEN 20

#define POSITION_FLAG_ALT    0x01
#define POSITION_FLAG_COURSE 0x02
#define POSITION_FLAG_SPEED  0x04
#define POSITION_FLAG_SATS   0x08

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

  size_t at = 0;
  out[at++] = POSITION_WIRE_VERSION;
  out[at++] = flags;
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
  out.lat_e7 = (int32_t)(((uint32_t)in[2] << 24) | ((uint32_t)in[3] << 16) |
                         ((uint32_t)in[4] << 8) | (uint32_t)in[5]);
  out.lon_e7 = (int32_t)(((uint32_t)in[6] << 24) | ((uint32_t)in[7] << 16) |
                         ((uint32_t)in[8] << 8) | (uint32_t)in[9]);
  const uint32_t secs = ((uint32_t)in[10] << 24) | ((uint32_t)in[11] << 16) |
                        ((uint32_t)in[12] << 8) | (uint32_t)in[13];
  out.fix_unix_ms = (uint64_t)secs * 1000ULL;
  out.accuracy_m = in[14];

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

// --- send path ---------------------------------------------------------------

// Set by provisioning. Empty disables reporting entirely, which is the default
// and the right default: a node that has not been told where to send its
// position must not guess.
inline RNS::Bytes position_gateway_hash;

// The heartbeat. §2 puts ten nodes at one report a minute inside budget and
// twenty-five nodes outside it, so this is a fleet-size decision rather than a
// taste one.
#ifndef POSITION_REPORT_INTERVAL_MS
#define POSITION_REPORT_INTERVAL_MS 60000UL
#endif
// Rolled per report so a fleet powered up together does not report in lockstep
// forever, the same reason TimeBeacon jitters its cadence.
#ifndef POSITION_REPORT_JITTER_MS
#define POSITION_REPORT_JITTER_MS 8000UL
#endif
// Movement that earns a report before the heartbeat is due.
//
// A stationary node still reports on the heartbeat: silence is not "unchanged"
// to a TAK client, it is a track going stale and vanishing off the map. But a
// node that *has* moved should not wait out the interval to say so, because
// that is exactly when its position matters.
#ifndef POSITION_MOVE_THRESHOLD_M
#define POSITION_MOVE_THRESHOLD_M 25
#endif
// 1e-7 degrees of latitude is about 1.11 cm. The same scale is applied to
// longitude, which is deliberately wrong away from the equator: a degree of
// longitude shortens with latitude, so a given movement produces a *larger*
// delta than this assumes, and the gate fires early rather than late. Erring
// toward reporting movement that did not happen is the safe direction; missing
// movement that did is not.
#define POSITION_MOVE_THRESHOLD_E7 ((int32_t)(POSITION_MOVE_THRESHOLD_M * 10000000L / 111320L))

// What a report costs on air, from the firmware's own model rather than a
// second copy of it. Declared rather than included: this header is pulled into
// the sketch, and the sketch is where the modem parameters live.
float packet_airtime_ms(uint16_t written);

struct PositionReportState {
  uint32_t next_due = 0;
  bool sent_any = false;
  int32_t last_lat_e7 = 0;
  int32_t last_lon_e7 = 0;
  uint32_t sent = 0;
  // Accounting, not enforcement. §7 step 5 exists so a cadence can be chosen
  // on evidence rather than discovered in an exercise: these boards are
  // deliberately not airtime-limited, and nothing here refuses to send. The
  // numbers are the deliverable.
  uint32_t bytes_sent = 0;
  float airtime_ms = 0.0f;
  uint32_t skipped_no_fix = 0;
  uint32_t skipped_no_path = 0;
  uint32_t path_requests = 0;
  uint32_t failures = 0;
  // Once, not every cadence: a node with no route to its gateway is a standing
  // condition, and TimeSync already learned that logging it as an event fills
  // the console on exactly the board that can least afford it.
  bool reported_waiting = false;
};

inline PositionReportState& position_report_state() {
  static PositionReportState state;
  return state;
}

inline bool position_has_moved(const NodePositionFix& fix,
                               const PositionReportState& st) {
  if (!st.sent_any) return true;
  const int32_t dlat = fix.lat_e7 - st.last_lat_e7;
  const int32_t dlon = fix.lon_e7 - st.last_lon_e7;
  const int32_t adlat = (dlat < 0) ? -dlat : dlat;
  const int32_t adlon = (dlon < 0) ? -dlon : dlon;
  return adlat >= POSITION_MOVE_THRESHOLD_E7 ||
         adlon >= POSITION_MOVE_THRESHOLD_E7;
}

// Share of the channel this node's position reporting is using, as a fraction
// of wall-clock time since boot. The figure §2 budgets against: ten nodes at
// one report a minute fits, twenty-five does not, and this says where a
// particular fleet actually sits rather than where the table predicts.
inline float position_report_duty_fraction() {
  const uint32_t up_s = node_uptime_seconds();
  if (up_s == 0) return 0.0f;
  return position_report_state().airtime_ms / ((float)up_s * 1000.0f);
}

inline void position_report_loop() {
  if (position_gateway_hash.size() == 0) return;

  PositionReportState& st = position_report_state();
  const uint32_t now = millis();
  const NodePositionFix& fix = node_position();

  const bool due = (st.next_due == 0) ||
                   ((uint32_t)(now - st.next_due) < (uint32_t)1 << 31);
  if (!due && !(fix.valid && position_has_moved(fix, st))) return;

  st.next_due = now + POSITION_REPORT_INTERVAL_MS +
                (uint32_t)random(POSITION_REPORT_JITTER_MS);

  // Nothing to say. Deliberately not sending a last known position: a marker
  // on a map is a claim about now, and repeating a lapsed fix is how a search
  // team ends up at somewhere the node used to be.
  if (!fix.valid || node_position_is_stale(fix)) { st.skipped_no_fix++; return; }

  if (!RNS::Transport::has_path(position_gateway_hash)) {
    RNS::Transport::request_path(position_gateway_hash);
    st.path_requests++;
    st.skipped_no_path++;
    if (!st.reported_waiting) {
      st.reported_waiting = true;
      printf("[position] no path to gateway <%s> yet, requesting one\n",
             position_gateway_hash.toHex().substr(0, 16).c_str());
    }
    return;
  }
  RNS::Identity gateway_identity = RNS::Identity::recall(position_gateway_hash);
  if (!gateway_identity) {
    st.skipped_no_path++;
    return;
  }
  st.reported_waiting = false;

  uint8_t wire[POSITION_WIRE_MAX_LEN];
  const size_t len = position_report_encode(fix, wire, sizeof(wire));
  if (len == 0) { st.failures++; return; }

  // A Packet, not a Link.
  //
  // Link establishment measured about eight kilobytes of transient heap on the
  // OZD fixture on 2026-09-06 -- the deepest single allocation on that board,
  // against roughly twenty-three free. Paying that every minute to deliver
  // twenty bytes would be the most expensive thing the node does, and a
  // position report needs neither a session nor a reply. A single packet to a
  // SINGLE destination is already encrypted to the gateway's identity.
  RNS::Destination gateway(gateway_identity,
                           RNS::Type::Destination::OUT,
                           RNS::Type::Destination::SINGLE,
                           RNS::Type::Transport::APP_NAME, "position.report");
  RNS::Packet packet(gateway, RNS::Bytes(wire, len));
  packet.send();
  st.sent++;
  st.bytes_sent += (uint32_t)len;
  st.airtime_ms += packet_airtime_ms((uint16_t)len);
  st.sent_any = true;
  st.last_lat_e7 = fix.lat_e7;
  st.last_lon_e7 = fix.lon_e7;
}

#else   // !HAS_RNS

inline void position_report_loop() {}

#endif  // HAS_RNS
