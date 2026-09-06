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

// Where this node thinks it is, and who told it.
//
// TAKCapability.md §7 step 1. The firmware owns the encoding and the send path;
// it does not own the receiver. A responder carries a phone with GNSS, a
// battery and a screen, and Columba already holds a mesh identity and signs
// with it -- that is the first source. An unattended relay on a hillside has no
// phone, and wants a GP-02 of its own (OnboardGNSS.md). Both produce the same
// thing: a fix, with a time and an accuracy attached.
//
// One seam, so that adding the second source later costs a source
// implementation rather than a redesign. Nothing downstream of here should ever
// ask where a position came from in order to decide what to do with it.

#include <Arduino.h>

#if defined(HAS_RNS)

#include <string.h>
#include "NodeStatus.h"

// Sources are registered once at startup and never removed, so a fixed array
// costs nothing and cannot fragment the heap. Four is more than a node has:
// today a phone, later a GNSS module, and a surveyed position for a fixed
// installation that knows where it was bolted down.
#ifndef NODE_POSITION_MAX_SOURCES
#define NODE_POSITION_MAX_SOURCES 4
#endif

// How long a fix may be used after it was taken.
//
// This is the number ATAK cares about. A CoT event carries a `stale` attribute
// and a client drops a track it can no longer trust, so a marker on the map is
// a claim about *now* -- and a position from ten minutes ago presented as
// current is worse than no marker at all, because it is believed. §2 puts our
// position cadence in minutes rather than seconds, so two minutes is a couple
// of missed reports, not a mis-set constant.
#ifndef NODE_POSITION_STALE_MS
#define NODE_POSITION_STALE_MS 120000UL
#endif

// A fix whose own clock disagrees with ours by more than this is not usable for
// a timestamped report. It is kept and reported as such rather than discarded:
// knowing a source is producing nonsense is worth more than silently having
// nothing.
#ifndef NODE_POSITION_MAX_CLOCK_SKEW_MS
#define NODE_POSITION_MAX_CLOCK_SKEW_MS 30000ULL
#endif

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

// A source is polled; it does not push. Pull keeps ownership of the buffer here
// and means a source that has stopped answering simply stops producing fixes,
// rather than leaving a stale one behind that nothing clears.
//
// A plain function pointer, not std::function: this is called from the main
// loop on a board that has run out of heap before, and std::function allocates.
using NodePositionPoll = bool (*)(NodePositionFix& out);

struct NodePositionSource {
  const char* name = nullptr;
  NodePositionKind kind = NodePositionKind::NONE;
  // Lower wins when several sources have a usable fix. Not a quality score --
  // a deliberate ordering, so behaviour is predictable rather than emergent.
  uint8_t rank = 255;
  NodePositionPoll poll = nullptr;
  // Counters, because a source that is registered but never produces anything
  // looks exactly like one that was never registered, and that difference is
  // the whole diagnosis. The same lesson as [timesync] reporting why it is
  // waiting.
  uint32_t fixes = 0;
  uint32_t rejected_stale = 0;
  uint32_t rejected_skew = 0;
};

inline NodePositionSource node_position_sources[NODE_POSITION_MAX_SOURCES];
inline uint8_t node_position_source_count = 0;

// The most recent fix accepted from any source, and which one.
inline NodePositionFix node_position_current;

inline bool node_position_register(const char* name, NodePositionKind kind,
                                   uint8_t rank, NodePositionPoll poll) {
  if (poll == nullptr || name == nullptr) return false;
  if (node_position_source_count >= NODE_POSITION_MAX_SOURCES) {
    printf("[position] cannot register '%s': %u source slots are all taken\n",
           name, (unsigned)NODE_POSITION_MAX_SOURCES);
    return false;
  }
  NodePositionSource& s = node_position_sources[node_position_source_count++];
  s.name = name;
  s.kind = kind;
  s.rank = rank;
  s.poll = poll;
  printf("[position] source '%s' registered at rank %u\n", name, (unsigned)rank);
  return true;
}

// Age of a fix in milliseconds, wrap-corrected. Uses the monotonic stamp, not
// the wall clock, so it is meaningful on a node that has never been told the
// time.
inline uint32_t node_position_age_ms(const NodePositionFix& fix) {
  if (!fix.valid) return UINT32_MAX;
  return (uint32_t)(millis() - fix.received_ms);
}

inline bool node_position_is_stale(const NodePositionFix& fix) {
  return !fix.valid || node_position_age_ms(fix) >= NODE_POSITION_STALE_MS;
}

// True when the fix can carry an honest timestamp: the source had a clock, we
// have a clock, and they agree. A fix that fails this is still a position --
// it just cannot be published as a timestamped observation, which is what a
// CoT event is.
inline bool node_position_time_is_trustworthy(const NodePositionFix& fix) {
  if (!fix.valid || fix.fix_unix_ms == 0) return false;
  if (!RNS::Utilities::OS::wall_time_known()) return false;
  const uint64_t ours = RNS::Utilities::OS::wall_time_millis();
  const uint64_t skew = (ours > fix.fix_unix_ms) ? (ours - fix.fix_unix_ms)
                                                 : (fix.fix_unix_ms - ours);
  return skew <= NODE_POSITION_MAX_CLOCK_SKEW_MS;
}

// Poll every source and keep the best usable fix.
//
// "Best" is the lowest rank among sources with a fresh fix, ties broken by
// which fix is newer. Deliberately not a quality metric: an operator who moves
// a node between sources should be able to predict what it will report.
inline void node_position_loop() {
  const uint32_t now = millis();
  const NodePositionSource* chosen = nullptr;
  NodePositionFix best;

  for (uint8_t i = 0; i < node_position_source_count; ++i) {
    NodePositionSource& s = node_position_sources[i];
    NodePositionFix fix;
    if (s.poll == nullptr || !s.poll(fix) || !fix.valid) continue;

    fix.kind = s.kind;
    // A source that reports no stamp of its own gets one now: it is being read
    // live, so "when we read it" is the honest answer. A source that does
    // stamp its fixes keeps its own, which is what makes a relayed position
    // from a phone age correctly rather than looking fresh on arrival.
    if (fix.received_ms == 0) fix.received_ms = now;

    if (node_position_is_stale(fix)) { s.rejected_stale++; continue; }
    if (fix.fix_unix_ms != 0 && !node_position_time_is_trustworthy(fix)) {
      s.rejected_skew++;
      // Not dropped. A position whose clock we distrust is still where the node
      // is; step 2 decides whether it may be published with a timestamp.
      fix.fix_unix_ms = 0;
    }
    s.fixes++;

    if (chosen == nullptr || s.rank < chosen->rank ||
        (s.rank == chosen->rank &&
         node_position_age_ms(fix) < node_position_age_ms(best))) {
      chosen = &s;
      best = fix;
    }
  }

  if (chosen != nullptr) {
    node_position_current = best;
  }
  else if (node_position_is_stale(node_position_current)) {
    // Let it lapse rather than holding the last known position forever. A node
    // that has stopped hearing from its receiver does not know where it is,
    // and saying so is the useful answer.
    node_position_current = NodePositionFix{};
  }
}

// The current fix, or an invalid one. Callers must check `valid` -- there is no
// sentinel coordinate, because every coordinate is somewhere real and null
// island is a place people have actually been sent.
inline const NodePositionFix& node_position() {
  return node_position_current;
}

inline const char* node_position_kind_name(NodePositionKind kind) {
  switch (kind) {
    case NodePositionKind::PHONE: return "phone";
    case NodePositionKind::GNSS:  return "gnss";
    case NodePositionKind::FIXED: return "fixed";
    default:                      return "none";
  }
}

#else   // !HAS_RNS

inline void node_position_loop() {}

#endif  // HAS_RNS
