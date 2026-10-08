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

// Reports a board keeps while no gateway is in reach (T2, LXMF reach), until
// they leave as one batch (TelemetryBatchCodec.h). Pure: no Arduino, radio or
// Reticulum, a fixed buffer allocated with the object, nothing allocated after.
//
// Sized so that whatever is kept always encodes into one batch: the batch's
// header and each entry's header are counted as reports are kept, and the
// oldest give way when the next would not fit -- the newest reports matter
// most, and the batch says some were dropped.
//
// Each report keeps the board's clock when it was taken, when the clock was
// set, and its uptime in any case; at composition an entry is absolute (unix
// seconds) if the clock was set when it was taken, and otherwise counted back
// from the moment of composition.

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "TelemetryBatchCodec.h"

#define TELEMETRY_KEPT_MAX_ENTRIES 64

class TelemetryKept {
public:
  // Keep one report taken now. `now_unix` is 0 when the clock is not set.
  // False only for a report no batch could carry.
  bool keep(const uint8_t* report, size_t len, uint32_t now_ms, uint32_t now_unix) {
    if (report == nullptr || len == 0 || len > TELEMETRY_BATCH_ENTRY_MAX) return false;
    while (_count > 0 && (_count >= TELEMETRY_KEPT_MAX_ENTRIES ||
                          encoded_len() + TELEMETRY_BATCH_ENTRY_HEADER + len > TELEMETRY_BATCH_MAX_LEN)) {
      drop_oldest();
    }
    Slot& s = _slots[_count++];
    s.taken_ms = now_ms;
    s.taken_unix = now_unix;
    s.offset = (uint16_t)_used;
    s.len = (uint16_t)len;
    memcpy(_data + _used, report, len);
    _used += len;
    return true;
  }

  size_t count() const { return _count; }
  bool empty() const { return _count == 0; }
  bool dropped() const { return _dropped; }
  uint32_t oldest_ms() const { return _count ? _slots[0].taken_ms : 0; }

  // The batch's length if composed now.
  size_t encoded_len() const {
    return TELEMETRY_BATCH_HEADER_LEN + _count * TELEMETRY_BATCH_ENTRY_HEADER + _used;
  }

  // The entries for telemetry_batch_encode(), oldest first, pointing into this
  // object's buffer: valid until the next keep() or clear().
  size_t entries(BatchEntry* out, size_t max, uint32_t now_ms) const {
    size_t n = 0;
    for (size_t i = 0; i < _count && n < max; ++i, ++n) {
      const Slot& s = _slots[i];
      out[n].report = _data + s.offset;
      out[n].len = s.len;
      if (s.taken_unix != 0) {
        out[n].time_kind = BATCH_TIME_ABSOLUTE;
        out[n].time = s.taken_unix;
      } else {
        out[n].time_kind = BATCH_TIME_RELATIVE;
        out[n].time = (now_ms - s.taken_ms) / 1000u;
      }
    }
    return n;
  }

  void clear() { _count = 0; _used = 0; _dropped = false; }

private:
  struct Slot {
    uint32_t taken_ms;
    uint32_t taken_unix;
    uint16_t offset;
    uint16_t len;
  };

  void drop_oldest() {
    const size_t len = _slots[0].len;
    memmove(_data, _data + len, _used - len);
    _used -= len;
    for (size_t i = 1; i < _count; ++i) {
      _slots[i - 1] = _slots[i];
      _slots[i - 1].offset = (uint16_t)(_slots[i - 1].offset - len);
    }
    --_count;
    _dropped = true;
  }

  uint8_t _data[TELEMETRY_BATCH_MAX_LEN];
  Slot _slots[TELEMETRY_KEPT_MAX_ENTRIES];
  size_t _count = 0;
  size_t _used = 0;
  bool _dropped = false;
};
