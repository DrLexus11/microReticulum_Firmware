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

// A board's detail report: what it runs, its interfaces, its radio, its
// propagation store and who it hears (TAKDeliveryPlan.md, telemetry T4/T5).
// Sent every half hour beside the five-minute health report (TelemetryCodec.h),
// to the same gateway destination; the first byte tells them apart (0x01 is the
// health report, 0x21 this one). Pure: no Arduino, radio or Reticulum, nothing
// allocated. Pinned against tools/telemetry_detail_codec.py by
// tests/fixtures/telemetry_detail_v1.json.
//
// Big-endian, fixed fields first, then lists:
//
//   off  size  field
//   0    1     kind_version  TELEMETRY_DETAIL_WIRE_VERSION (0x21)
//   1    1     flags         DETAIL_FLAG_*
//   2    4     sender_id     u32  four bytes of the sender's identity hash
//   6    4     uptime_s      u32
//   10   4     fw_hash       first four bytes of the running image's SHA-256
//   14   2     fw_version    u16  major << 8 | minor
//   16   1     env_len       u8   then env_len bytes: the build environment's name
//   --         if_count      u8   then if_count x 10 bytes:
//                kind u8 (DETAIL_IF_*), state u8 (bit 0: up),
//                rx u32, tx u32 -- bytes since boot, as the interface counts
//                them (cumulative, so the backend takes rates and a restart
//                reads as a counter reset)
//   --         FLAG_RADIO:   rssi i8 dBm, snr i8 (quarter dB), noise i8 dBm,
//                            utilisation u8 %, airtime u8 %
//   --         FLAG_PROPAGATION: store_msgs u16, store_kb u16, peers u8,
//                            sync_ok u16, sync_fail u16, last_sync_min u16
//                            (0xFFFF: never)
//   --         nb_count      u8   then nb_count x 7 bytes:
//                id u32 (four bytes of the neighbour's identity hash),
//                kind u8 (DETAIL_IF_* it is heard on), rssi i8 dBm
//                (DETAIL_RSSI_UNKNOWN off radio), heard_min u8 (minutes ago)
//   --         FLAG_SYSTEM (after the neighbours, so a decoder that predates it
//              reads everything else unchanged; 20 bytes):
//                temp_c i8 (chip temperature; -128 unknown),
//                lora_rx u32, lora_tx u32 (LoRa frames since boot; a split
//                packet is two),
//                lora_crc u32 (frames dropped on a CRC error since boot;
//                0xFFFFFFFF where the radio does not count them),
//                time_source u8 (the clock's source: OS::WallTimeSource,
//                0 unknown), time_age_min u16 (since the clock was last
//                adopted; 0xFFFF never),
//                ifac_rejected u32 (packets refused for a wrong network key
//                since boot; 0xFFFFFFFF until the library counts them)
//
// The whole report fits one encrypted Reticulum packet: past
// TELEMETRY_DETAIL_WIRE_MAX_LEN the encoder drops neighbours and sets
// DETAIL_FLAG_NEIGHBOURS_TRUNCATED rather than fail. Counts saturate.

#include <cstddef>
#include <cstdint>
#include <cstring>

#define TELEMETRY_DETAIL_WIRE_VERSION 0x21
#define TELEMETRY_DETAIL_WIRE_MAX_LEN 380
#define DETAIL_ENV_MAX                32
#define DETAIL_MAX_INTERFACES         12
#define DETAIL_MAX_NEIGHBOURS         48

#define DETAIL_FLAG_RADIO                0x01
#define DETAIL_FLAG_PROPAGATION          0x02
#define DETAIL_FLAG_NEIGHBOURS_TRUNCATED 0x04
#define DETAIL_FLAG_SYSTEM               0x08

#define DETAIL_IF_OTHER      0
#define DETAIL_IF_LORA       1
#define DETAIL_IF_BLE_PEER   2
#define DETAIL_IF_ESPNOW     3
#define DETAIL_IF_TCP_SERVER 4
#define DETAIL_IF_TCP_CLIENT 5
#define DETAIL_IF_UDP        6
#define DETAIL_IF_AUTO       7
#define DETAIL_IF_SERIAL     8
#define DETAIL_IF_HALOW      9

#define DETAIL_RSSI_UNKNOWN  (-128)
#define DETAIL_NEVER         0xFFFFFFFFu
#define DETAIL_UNKNOWN32     0xFFFFFFFFu
#define DETAIL_TEMP_UNKNOWN  (-128)
#define DETAIL_SYSTEM_LEN    20

struct DetailInterface {
  uint8_t kind = DETAIL_IF_OTHER;
  bool up = false;
  uint32_t rx_bytes = 0;
  uint32_t tx_bytes = 0;
};

struct DetailNeighbour {
  uint32_t id = 0;
  uint8_t kind = DETAIL_IF_OTHER;
  int8_t rssi = DETAIL_RSSI_UNKNOWN;
  uint32_t heard_s = 0;          // encoded as whole minutes, saturating at 255
};

struct NodeDetail {
  uint32_t sender_id = 0;
  uint32_t uptime_s = 0;
  uint8_t fw_hash[4] = {0, 0, 0, 0};
  uint16_t fw_version = 0;
  char env[DETAIL_ENV_MAX + 1] = {0};

  uint8_t if_count = 0;
  DetailInterface interfaces[DETAIL_MAX_INTERFACES];

  bool radio_known = false;
  int8_t rssi = DETAIL_RSSI_UNKNOWN;
  int8_t snr_q = 0;              // quarter dB
  int8_t noise = DETAIL_RSSI_UNKNOWN;
  uint8_t utilisation_pct = 0;
  uint8_t airtime_pct = 0;

  bool propagation_known = false;
  uint32_t store_messages = 0;
  uint32_t store_bytes = 0;      // encoded as whole KB
  uint8_t pn_peers = 0;
  uint32_t sync_ok = 0;
  uint32_t sync_fail = 0;
  uint32_t last_sync_s = DETAIL_NEVER;  // encoded as whole minutes; never = 0xFFFF

  uint8_t nb_count = 0;
  DetailNeighbour neighbours[DETAIL_MAX_NEIGHBOURS];
  bool neighbours_truncated = false;

  bool system_known = false;
  int8_t temperature_c = DETAIL_TEMP_UNKNOWN;
  uint32_t lora_rx = 0;
  uint32_t lora_tx = 0;
  uint32_t lora_crc_errors = DETAIL_UNKNOWN32;
  uint8_t time_source = 0;
  uint32_t time_age_s = DETAIL_NEVER;        // encoded as whole minutes; never = 0xFFFF
  uint32_t ifac_rejected = DETAIL_UNKNOWN32;
};

// An interface's kind from its name, as the firmware names them
// ("LoRaInterface", "TCPServerInterface", ...). Pure, so it is host-tested.
inline uint8_t detail_kind_of(const char* name) {
  if (name == nullptr) return DETAIL_IF_OTHER;
  struct Rule { const char* fragment; uint8_t kind; };
  static const Rule rules[] = {
    {"LoRa", DETAIL_IF_LORA},       {"BLEPeer", DETAIL_IF_BLE_PEER},
    {"ESPNow", DETAIL_IF_ESPNOW},   {"TCPServer", DETAIL_IF_TCP_SERVER},
    {"TCPClient", DETAIL_IF_TCP_CLIENT}, {"UDP", DETAIL_IF_UDP},
    {"Auto", DETAIL_IF_AUTO},       {"Serial", DETAIL_IF_SERIAL},
    {"KISS", DETAIL_IF_SERIAL},     {"HaLow", DETAIL_IF_HALOW},
  };
  for (const Rule& r : rules) {
    if (strstr(name, r.fragment) != nullptr) return r.kind;
  }
  return DETAIL_IF_OTHER;
}

inline uint16_t detail_sat16(uint32_t v) { return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v; }
inline uint8_t detail_sat8(uint32_t v) { return v > 0xFFu ? 0xFFu : (uint8_t)v; }
inline uint8_t detail_pct(uint32_t v) { return v > 100u ? 100u : (uint8_t)v; }

inline void detail_put16(uint8_t* out, size_t& at, uint16_t v) {
  out[at++] = (uint8_t)(v >> 8);
  out[at++] = (uint8_t)v;
}
inline void detail_put32(uint8_t* out, size_t& at, uint32_t v) {
  out[at++] = (uint8_t)(v >> 24); out[at++] = (uint8_t)(v >> 16);
  out[at++] = (uint8_t)(v >> 8);  out[at++] = (uint8_t)v;
}
inline uint16_t detail_get16(const uint8_t* in) { return (uint16_t)(((uint16_t)in[0] << 8) | in[1]); }
inline uint32_t detail_get32(const uint8_t* in) {
  return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) | (uint32_t)in[3];
}

// The report's bytes, or 0 when `out` cannot hold even the fixed part. The
// neighbour list is cut to what fits in min(out_len, MAX_LEN), flagged.
inline size_t telemetry_detail_encode(const NodeDetail& d, uint8_t* out, size_t out_len) {
  const size_t limit = out_len < TELEMETRY_DETAIL_WIRE_MAX_LEN ? out_len : TELEMETRY_DETAIL_WIRE_MAX_LEN;
  size_t env_len = strnlen(d.env, DETAIL_ENV_MAX);
  const uint8_t ifs = d.if_count > DETAIL_MAX_INTERFACES ? DETAIL_MAX_INTERFACES : d.if_count;
  size_t fixed = 17 + env_len + 1 + (size_t)ifs * 10 + (d.radio_known ? 5 : 0) +
                 (d.propagation_known ? 11 : 0) + 1 + (d.system_known ? DETAIL_SYSTEM_LEN : 0);
  if (out == nullptr || fixed > limit) return 0;
  const uint8_t wanted = d.nb_count > DETAIL_MAX_NEIGHBOURS ? DETAIL_MAX_NEIGHBOURS : d.nb_count;
  uint8_t nbs = wanted;
  while (nbs > 0 && fixed + (size_t)nbs * 7 > limit) --nbs;
  uint8_t flags = 0;
  if (d.radio_known)       flags |= DETAIL_FLAG_RADIO;
  if (d.propagation_known) flags |= DETAIL_FLAG_PROPAGATION;
  if (nbs < wanted || d.neighbours_truncated) flags |= DETAIL_FLAG_NEIGHBOURS_TRUNCATED;
  if (d.system_known)      flags |= DETAIL_FLAG_SYSTEM;

  size_t at = 0;
  out[at++] = TELEMETRY_DETAIL_WIRE_VERSION;
  out[at++] = flags;
  detail_put32(out, at, d.sender_id);
  detail_put32(out, at, d.uptime_s);
  for (int i = 0; i < 4; ++i) out[at++] = d.fw_hash[i];
  detail_put16(out, at, d.fw_version);
  out[at++] = (uint8_t)env_len;
  memcpy(out + at, d.env, env_len);
  at += env_len;
  out[at++] = ifs;
  for (uint8_t i = 0; i < ifs; ++i) {
    const DetailInterface& f = d.interfaces[i];
    out[at++] = f.kind;
    out[at++] = f.up ? 0x01 : 0x00;
    detail_put32(out, at, f.rx_bytes);
    detail_put32(out, at, f.tx_bytes);
  }
  if (d.radio_known) {
    out[at++] = (uint8_t)d.rssi;
    out[at++] = (uint8_t)d.snr_q;
    out[at++] = (uint8_t)d.noise;
    out[at++] = detail_pct(d.utilisation_pct);
    out[at++] = detail_pct(d.airtime_pct);
  }
  if (d.propagation_known) {
    detail_put16(out, at, detail_sat16(d.store_messages));
    detail_put16(out, at, detail_sat16(d.store_bytes / 1024));
    out[at++] = d.pn_peers;
    detail_put16(out, at, detail_sat16(d.sync_ok));
    detail_put16(out, at, detail_sat16(d.sync_fail));
    const uint32_t minutes = d.last_sync_s == DETAIL_NEVER ? 0xFFFFu : d.last_sync_s / 60;
    detail_put16(out, at, d.last_sync_s == DETAIL_NEVER ? 0xFFFFu : (minutes >= 0xFFFFu ? 0xFFFEu : (uint16_t)minutes));
  }
  out[at++] = nbs;
  for (uint8_t i = 0; i < nbs; ++i) {
    const DetailNeighbour& n = d.neighbours[i];
    detail_put32(out, at, n.id);
    out[at++] = n.kind;
    out[at++] = (uint8_t)n.rssi;
    out[at++] = detail_sat8(n.heard_s / 60);
  }
  if (d.system_known) {
    out[at++] = (uint8_t)d.temperature_c;
    detail_put32(out, at, d.lora_rx);
    detail_put32(out, at, d.lora_tx);
    detail_put32(out, at, d.lora_crc_errors);
    out[at++] = d.time_source;
    const uint32_t minutes = d.time_age_s == DETAIL_NEVER ? 0xFFFFu : d.time_age_s / 60;
    detail_put16(out, at, d.time_age_s == DETAIL_NEVER ? 0xFFFFu : (minutes >= 0xFFFFu ? 0xFFFEu : (uint16_t)minutes));
    detail_put32(out, at, d.ifac_rejected);
  }
  return at;
}

// The inverse. False for anything malformed: a wrong first byte, a length
// that ends inside a field, more entries than the arrays hold. Bytes past the
// neighbour list are ignored, so a later version can append.
inline bool telemetry_detail_decode(const uint8_t* in, size_t len, NodeDetail& out) {
  if (in == nullptr || len < 18 || in[0] != TELEMETRY_DETAIL_WIRE_VERSION) return false;
  out = NodeDetail{};
  const uint8_t flags = in[1];
  out.sender_id = detail_get32(in + 2);
  out.uptime_s = detail_get32(in + 6);
  memcpy(out.fw_hash, in + 10, 4);
  out.fw_version = detail_get16(in + 14);
  size_t at = 16;
  const uint8_t env_len = in[at++];
  if (env_len > DETAIL_ENV_MAX || at + env_len + 1 > len) return false;
  memcpy(out.env, in + at, env_len);
  out.env[env_len] = 0;
  at += env_len;
  out.if_count = in[at++];
  if (out.if_count > DETAIL_MAX_INTERFACES || at + (size_t)out.if_count * 10 > len) return false;
  for (uint8_t i = 0; i < out.if_count; ++i) {
    DetailInterface& f = out.interfaces[i];
    f.kind = in[at];
    f.up = (in[at + 1] & 0x01) != 0;
    f.rx_bytes = detail_get32(in + at + 2);
    f.tx_bytes = detail_get32(in + at + 6);
    at += 10;
  }
  if (flags & DETAIL_FLAG_RADIO) {
    if (at + 5 > len) return false;
    out.radio_known = true;
    out.rssi = (int8_t)in[at];
    out.snr_q = (int8_t)in[at + 1];
    out.noise = (int8_t)in[at + 2];
    out.utilisation_pct = in[at + 3];
    out.airtime_pct = in[at + 4];
    at += 5;
  }
  if (flags & DETAIL_FLAG_PROPAGATION) {
    if (at + 11 > len) return false;
    out.propagation_known = true;
    out.store_messages = detail_get16(in + at);
    out.store_bytes = (uint32_t)detail_get16(in + at + 2) * 1024u;
    out.pn_peers = in[at + 4];
    out.sync_ok = detail_get16(in + at + 5);
    out.sync_fail = detail_get16(in + at + 7);
    const uint16_t minutes = detail_get16(in + at + 9);
    out.last_sync_s = minutes == 0xFFFFu ? DETAIL_NEVER : (uint32_t)minutes * 60u;
    at += 11;
  }
  if (at + 1 > len) return false;
  out.nb_count = in[at++];
  if (out.nb_count > DETAIL_MAX_NEIGHBOURS || at + (size_t)out.nb_count * 7 > len) return false;
  for (uint8_t i = 0; i < out.nb_count; ++i) {
    DetailNeighbour& n = out.neighbours[i];
    n.id = detail_get32(in + at);
    n.kind = in[at + 4];
    n.rssi = (int8_t)in[at + 5];
    n.heard_s = (uint32_t)in[at + 6] * 60u;
    at += 7;
  }
  out.neighbours_truncated = (flags & DETAIL_FLAG_NEIGHBOURS_TRUNCATED) != 0;
  if (flags & DETAIL_FLAG_SYSTEM) {
    if (at + DETAIL_SYSTEM_LEN > len) return false;
    out.system_known = true;
    out.temperature_c = (int8_t)in[at];
    out.lora_rx = detail_get32(in + at + 1);
    out.lora_tx = detail_get32(in + at + 5);
    out.lora_crc_errors = detail_get32(in + at + 9);
    out.time_source = in[at + 13];
    const uint16_t minutes = detail_get16(in + at + 14);
    out.time_age_s = minutes == 0xFFFFu ? DETAIL_NEVER : (uint32_t)minutes * 60u;
    out.ifac_rejected = detail_get32(in + at + 16);
    at += DETAIL_SYSTEM_LEN;
  }
  return true;
}
