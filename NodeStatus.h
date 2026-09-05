// One place to ask what this node is doing.
//
// The answers below already existed, scattered: the display read some of them
// out of RNode globals, Provisioning.cpp wrapped others in metric lambdas, the
// diagnostic pages recomputed a third set inline, and the mesh counts existed
// nowhere at all. Three renderings of the same question is how they drift.
//
// This gathers them once. The OLED is the first caller; the Rev 2 mezzanine
// will be the second and needs exactly the same set, and /page/device.mu is the
// obvious third. Nothing here computes anything a caller could not have
// computed itself -- it is a gathering layer, not a new subsystem.
#pragma once

#include <Arduino.h>

// microReticulum before Config.h, deliberately. Config.h defines MTU as a
// preprocessor constant and Type.h uses MTU as an identifier, so the reverse
// order fails to parse inside the library. The .ino gets away with its own
// ordering; a new translation unit has to be explicit about it.
#if defined(HAS_RNS)
#include <microReticulum.h>
#include <microReticulum/Transport.h>
#include <microReticulum/Reticulum.h>
#include <microReticulum/Utilities/OS.h>
#endif

// Boards.h, not Config.h. Config.h *defines* the firmware's globals rather than
// declaring them, so including it from a second translation unit produces a
// duplicate symbol for every one of them at link time. Provisioning.cpp solved
// this the same way: take Boards.h for the feature macros, and reach the
// globals by extern.
#include "Boards.h"

// --- announce census ---------------------------------------------------------
//
// Counts distinct destinations by what they announce themselves as, using the
// taxonomy Columba's network view already uses (NodeType in AnnounceEvent.kt),
// so the same mesh reads the same way on the phone and on the panel:
//
//   PEER   lxmf.delivery      a node we can message with
//   RELAY  lxmf.propagation   a store-and-forward relay
//   NOMAD  nomadnetwork.node  a page-serving node
//
// Distinctness matters: a node that announces every 30 minutes must not count
// as a new peer each time. Full 16-byte hashes for every destination would be
// real memory on a WROOM-32 with no PSRAM, so this keeps 32 bits of each hash.
// A collision undercounts by one and is invisible; storing 4x the bytes to
// avoid a one-in-four-billion miscount on a display would be the wrong trade.
enum NodeCensusKind : uint8_t {
  NODE_CENSUS_PEER = 0,
  NODE_CENSUS_RELAY = 1,
  NODE_CENSUS_NOMAD = 2,
  // Distinct *identities* heard announcing, whatever they announced. This is
  // the one that means "nodes": the path table counts destinations, and every
  // node announces several of them -- transport, probe, management, nomadnet,
  // lxmf delivery, propagation -- so its size reads four to eight times the
  // number of actual devices. Sixteen paths on the bench is four boards.
  NODE_CENSUS_NODE = 3,
  NODE_CENSUS_KINDS = 4,
};

#ifndef NODE_CENSUS_CAPACITY
#define NODE_CENSUS_CAPACITY 96
#endif

struct NodeCensus {
  uint32_t prefix[NODE_CENSUS_CAPACITY];
  uint8_t kind[NODE_CENSUS_CAPACITY];
  uint16_t used = 0;
  uint16_t counts[NODE_CENSUS_KINDS] = {0, 0, 0, 0};
  // Non-zero once the table is full: the counts have stopped rising and the
  // panel should say so rather than report a number it knows is short.
  uint16_t overflowed = 0;
};

inline NodeCensus& node_census() {
  static NodeCensus census;
  return census;
}

inline void node_census_record(NodeCensusKind kind, const uint8_t* hash, size_t length) {
  if (hash == nullptr || length < 4) return;
  NodeCensus& c = node_census();
  const uint32_t prefix = ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
                          ((uint32_t)hash[2] << 8) | (uint32_t)hash[3];
  for (uint16_t i = 0; i < c.used; ++i) {
    if (c.prefix[i] == prefix && c.kind[i] == (uint8_t)kind) return;
  }
  if (c.used >= NODE_CENSUS_CAPACITY) { c.overflowed++; return; }
  c.prefix[c.used] = prefix;
  c.kind[c.used] = (uint8_t)kind;
  c.used++;
  c.counts[kind]++;
}

// --- abnormal restarts -------------------------------------------------------
//
// BOOTS already survives a reset in RTC memory, but it resets on power loss and
// says nothing about *why* a board restarted. These two do, and they persist to
// the filesystem so a board that has been unplugged still knows its own
// history. Written only when one actually happens, which on a healthy node is
// never.
inline uint32_t node_crash_count = 0;   // watchdog or brownout
inline uint32_t node_panic_count = 0;   // firmware exception

// --- the gathered view -------------------------------------------------------

struct NodeStatusView {
  const char* name = "";

  // Services this node offers, as opposed to interfaces it speaks over.
  bool rrc_hub = false;
  bool propagation = false;

  // present: compiled in and initialised at all.
  // active:  up and usable right now. The panel draws present-but-idle
  //          differently from absent, because "no BLE fitted" and "BLE fitted
  //          and dead" are different problems.
  bool lora_present = false, lora_active = false;
  bool ble_present = false, ble_active = false;
  bool wifi_present = false, wifi_active = false;
  bool espnow_present = false, espnow_active = false;
  // Enough of the ESP-NOW link and its recovery state machine to say what the
  // radio is doing, not merely whether it is up. On a board whose only way
  // home is a channel sweep, "scanning" and "attached" are the difference
  // between wait and intervene.
  uint8_t espnow_channel = 0;
  uint16_t espnow_peers = 0;
  const char* recovery_state = "";
  bool recovery_active = false;
  bool recovery_pinned = false;
  bool recovery_failed = false;
  uint8_t recovery_channel = 0;

  bool time_known = false;
  uint8_t stratum = 0;
  uint64_t unix_ms = 0;
  // Where the clock came from, as a three-character tag for the panel. A time
  // that is right and a time that is merely restored look identical otherwise,
  // and only one of them should be trusted.
  const char* time_source = "";

  uint16_t peers = 0;
  // Distinct identities heard, not path-table entries. See NODE_CENSUS_NODE.
  uint16_t nodes = 0;
  // Path-table size: destinations reachable, several per node. Kept for the
  // interfaces page, where "paths" is what it is called.
  uint16_t paths = 0;
  uint16_t relays = 0;
  uint16_t nomad = 0;
  bool census_full = false;

  uint32_t uptime_s = 0;
  uint32_t boots = 0;
  uint32_t crashes = 0;
  uint32_t panics = 0;

  // Every compiled interface reports itself up. Deliberately health, not
  // traffic: a quiet mesh is not a broken one, and a panel that cries wolf
  // overnight gets ignored by morning.
  bool interfaces_ok = false;
  // The first fitted interface that is not up, or null when all are.
  const char* down_interface = nullptr;
  // Participating in a mesh: a live radio and somewhere to reach.
  bool mesh_on = false;
  // Forwarding for others. A different question, and false on every
  // radio-less board by construction.
  bool relaying = false;
};

NodeStatusView node_status();

// Load the persisted abnormal-restart counters and, if this boot was itself
// abnormal, record it. Call once after the filesystem is mounted.
void node_restart_counts_record_boot();

// Register the announce handlers that feed the census. Call once, after
// Transport has started.
void node_census_begin();

// Seconds since boot, wrap-corrected.
uint32_t node_uptime_seconds();
