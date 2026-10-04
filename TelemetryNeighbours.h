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

// Who this board hears directly, for the detail report (TelemetryDetailCodec.h).
//
// Recorded as announces arrive, not read from the path table: on the boards the
// path table lives on flash, and walking 27 entries took 819 ms of the loop
// (Rev 1, 2026-10-04). An announce names the node that transmitted it without
// any lookup:
//
// - a rebroadcast announce (HEADER_2) carries the rebroadcaster's transport
//   identity hash -- the same four bytes the health report calls sender_id;
// - an announce heard at hops 0 (HEADER_1) comes from its originator, and the
//   originator's identity hash is the SHA-256 of the public key it carries.
//
// Pure: the caller hashes the public key and supplies the clock. A fixed table,
// allocated once; a full table replaces its stalest entry.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "TelemetryDetailCodec.h"

#define NEIGHBOUR_TABLE_SIZE DETAIL_MAX_NEIGHBOURS
#ifndef NEIGHBOUR_EXPIRY_MS
// Not heard for six hours: no longer a neighbour. The report's age saturates at
// 255 minutes, so anything between that and this reads as "over four hours".
#define NEIGHBOUR_EXPIRY_MS (6UL * 60UL * 60UL * 1000UL)
#endif

// Raw packet layout (Reticulum): flags, hops, then one or two 16-byte hashes,
// a context byte, the data. Flags: bit 6 header type, bits 0-1 packet type.
#define NEIGHBOUR_PKT_ANNOUNCE   0x01
#define NEIGHBOUR_HASH_LEN       16
#define NEIGHBOUR_PUBKEY_LEN     64

enum class AnnounceSource : uint8_t {
  None,          // not an announce, or not from a neighbour
  TransportId,   // `at` is the rebroadcaster's transport identity hash
  PublicKey,     // `at` is the originator's public key; hash it for the identity
};

// Where in `raw` the neighbour's identity is, if this packet names one.
inline AnnounceSource announce_neighbour(const uint8_t* raw, size_t len, size_t& at) {
  if (raw == nullptr || len < 2) return AnnounceSource::None;
  if ((raw[0] & 0x03) != NEIGHBOUR_PKT_ANNOUNCE) return AnnounceSource::None;
  const bool header_2 = (raw[0] & 0x40) != 0;
  if (header_2) {
    if (len < 2 + 2 * NEIGHBOUR_HASH_LEN + 1) return AnnounceSource::None;
    at = 2;
    return AnnounceSource::TransportId;
  }
  if (raw[1] != 0) return AnnounceSource::None;
  if (len < 2 + NEIGHBOUR_HASH_LEN + 1 + NEIGHBOUR_PUBKEY_LEN) return AnnounceSource::None;
  at = 2 + NEIGHBOUR_HASH_LEN + 1;
  return AnnounceSource::PublicKey;
}

inline uint32_t neighbour_id_of(const uint8_t* hash) {
  return ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) | ((uint32_t)hash[2] << 8) | (uint32_t)hash[3];
}

class NeighbourTable {
public:
  struct Entry {
    uint32_t id = 0;
    uint8_t kind = DETAIL_IF_OTHER;
    int8_t rssi = DETAIL_RSSI_UNKNOWN;
    uint32_t heard_ms = 0;
    bool used = false;
  };

  // A neighbour heard now. RSSI is kept from the last time it was known on the
  // same kind of interface; heard over another kind, it is unknown until known.
  void heard(uint32_t id, uint8_t kind, int8_t rssi, uint32_t now_ms) {
    Entry* slot = nullptr;
    Entry* free_slot = nullptr;
    Entry* stalest = nullptr;
    for (Entry& e : _entries) {
      if (!e.used) { if (free_slot == nullptr) free_slot = &e; continue; }
      if (e.id == id) { slot = &e; break; }
      if (stalest == nullptr || now_ms - e.heard_ms > now_ms - stalest->heard_ms) stalest = &e;
    }
    if (slot == nullptr) {
      slot = free_slot != nullptr ? free_slot : stalest;
      *slot = Entry{};
      slot->id = id;
      slot->kind = kind;
      slot->used = true;
    }
    if (rssi != DETAIL_RSSI_UNKNOWN) slot->rssi = rssi;
    else if (slot->kind != kind) slot->rssi = DETAIL_RSSI_UNKNOWN;
    slot->kind = kind;
    slot->heard_ms = now_ms;
  }

  // The live neighbours into the report, most recently heard first.
  void fill(NodeDetail& d, uint32_t now_ms) {
    d.nb_count = 0;
    d.neighbours_truncated = false;
    for (Entry& e : _entries) {
      if (!e.used) continue;
      const uint32_t age = now_ms - e.heard_ms;
      if (age > NEIGHBOUR_EXPIRY_MS) { e.used = false; continue; }
      DetailNeighbour& n = d.neighbours[d.nb_count++];
      n.id = e.id;
      n.kind = e.kind;
      n.rssi = e.rssi;
      n.heard_s = age / 1000u;
    }
    for (uint8_t i = 1; i < d.nb_count; ++i) {
      const DetailNeighbour n = d.neighbours[i];
      uint8_t j = i;
      while (j > 0 && d.neighbours[j - 1].heard_s > n.heard_s) { d.neighbours[j] = d.neighbours[j - 1]; --j; }
      d.neighbours[j] = n;
    }
  }

  size_t size() const {
    size_t n = 0;
    for (const Entry& e : _entries) n += e.used ? 1 : 0;
    return n;
  }

private:
  Entry _entries[NEIGHBOUR_TABLE_SIZE];
};
