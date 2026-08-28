// Bridging persistent RRC rooms to LXMF store-and-forward.
//
// WHY THIS EXISTS
//
// RRC is ephemeral by design, and for incident chat that is correct. The stated
// product is different: group command and central command over this mesh, a
// backbone. There "what did I miss while I was out of range?" is the entire
// question, and mobility guarantees it gets asked -- long-lived Links are the
// most path-sensitive part of this stack, so responders will drop and rejoin.
//
// The answer is deliberately not to give RRC a message history. The ecosystem
// already has store-and-forward, stock clients speak it, this node runs it, and
// it survives the hub going down. Adding a store to the hub would duplicate all
// of that and quietly break the ephemeral contract stock clients rely on.
//
// So: a provisioned set of rooms is bridged. Traffic accepted in a bridged room
// is additionally composed as an LXMF message to each member who is not
// currently connected, and handed to this node's own propagation store. Live
// members still receive the ordinary RRC fanout; the bridge is only for the
// absent. Ad-hoc rooms keep today's behaviour exactly.
//
// See docs/RRCRequirements.md §12c.
//
// THREE THINGS THAT SHAPE THE DESIGN
//
// Membership dies with the Link, so "who is absent" cannot be answered from
// hub state alone -- an absent member is precisely one with no session. The
// bridge therefore keeps its own roster of who has been seen in a bridged room,
// holding each member's public key so it can address them later. That key comes
// from the Link when the remote end identifies, which means the hub can reach a
// member it has met without ever having seen their announce.
//
// Composing costs a signature and an encryption per recipient, and it must not
// happen where room traffic is handled: that path runs in a Reticulum callback,
// where this project has already lost a board to a stack overflow. Publishing
// therefore only enqueues, and rrc_bridge_loop() does the cryptography one
// recipient at a time from the main loop.
//
// Loops are structurally impossible in this direction and it is worth being
// explicit about why: nothing here ever injects into RRC, and RRC ingress is
// only ever an envelope arriving on a Link. When the reverse direction is added
// -- LXMF replies appearing in the room -- loop prevention stops being free and
// has to be built, using the transient-id tracking LXMF already provides.

#pragma once

#if defined(RRC_HUB) && defined(LXMF_PROPAGATION_NODE)
#define RRC_LXMF_BRIDGE 1

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <microReticulum.h>

#include "RRCProtocol.h"

// Bounded, like everything else the network can influence. A deployment names
// a handful of standing rooms; the roster is what makes the bridge useful, so
// it is the larger of the two, and both are small enough to keep in RAM.
#ifndef RRC_BRIDGE_MAX_ROOMS
#define RRC_BRIDGE_MAX_ROOMS 4
#endif
#ifndef RRC_BRIDGE_MAX_MEMBERS
#define RRC_BRIDGE_MAX_MEMBERS 16
#endif

// Pending recipients waiting for their signature and encryption. Depth is in
// messages, not recipients; each queued message may fan out to the whole roster.
#ifndef RRC_BRIDGE_QUEUE_DEPTH
#define RRC_BRIDGE_QUEUE_DEPTH 8
#endif

// How much of the propagation store bridged room traffic may occupy.
//
// The store is shared with residents' personal mail, and a busy room would
// otherwise evict it: 128 messages is not many when one room can produce that
// in an afternoon. Bridged messages are evicted against this quota before the
// store's own cap is consulted, so room traffic can fill its share and no more.
//
// The quota is enforced against messages this node composed during the current
// uptime. A composed message is indistinguishable from any other blob once
// stored -- its source is inside the ciphertext -- so a reboot forgets which
// were ours and they revert to ordinary oldest-first eviction. The store stays
// bounded either way; only the fairness is lost, and only until the room is
// next busy.
#ifndef RRC_BRIDGE_STORE_QUOTA
#define RRC_BRIDGE_STORE_QUOTA (LXMF_PN_MAX_MESSAGES / 4)
#endif

// Recent room traffic kept so that a member joining a bridged room for the
// first time receives what was said before they arrived.
//
// This is the one thing 12c promised and the first implementation did not do:
// backfill only reached roster members who were absent when a message was
// sent, and a first-time joiner was never on the roster, so they got nothing.
//
// It does not give RRC a message history. Nothing here is ever served over
// RRC, and the ephemeral contract stock clients rely on is untouched -- the
// catch-up is composed as one LXMF message and travels the same path as the
// rest of the bridge. That distinction is the whole reason this is allowed to
// exist; keep it.
//
// The ring lives in PSRAM. 8 MB of it sits idle on both revisions while
// internal RAM is the scarce resource, and this is exactly the kind of bulk,
// non-DMA buffer that belongs there.
#ifndef RRC_BRIDGE_HISTORY_MAX
#define RRC_BRIDGE_HISTORY_MAX 32          // hard cap; the provisioned depth cannot exceed it
#endif
#ifndef RRC_BRIDGE_HISTORY_TEXT
#define RRC_BRIDGE_HISTORY_TEXT 288        // message text, per line
#endif
#ifndef RRC_BRIDGE_HISTORY_NICK
#define RRC_BRIDGE_HISTORY_NICK 40         // display name, per line
#endif

// Structured metadata for clients that merge live RRC and bridged LXMF into
// one conversation. Stock clients ignore unknown fields, so this costs them
// nothing and the human-readable body is unchanged.
//
// The identifier is versioned because the shape will change: message ids and
// sender identities are enough to deduplicate and order today, and per-sender
// signatures are the obvious next addition if rooms ever need end-to-end
// attribution rather than the hub's word for it.
//
// See docs/BridgeClientContract.md.
#ifndef RRC_BRIDGE_FIELD_TYPE
#define RRC_BRIDGE_FIELD_TYPE "rrc.bridge/1"
#endif
#define LXMF_FIELD_CUSTOM_TYPE 0xFB
#define LXMF_FIELD_CUSTOM_DATA 0xFC
#define LXMF_FIELD_CUSTOM_META 0xFD
// One catch-up message must still fit the advertised per-message transfer
// limit, so the digest is assembled newest-first under a byte budget and only
// then put back in order. Without this a deep history would compose a message
// the store is required to reject.
#ifndef RRC_BRIDGE_DIGEST_BUDGET
#define RRC_BRIDGE_DIGEST_BUDGET 3000
#endif

// How often the bridge announces the address it sends from, so recipients can
// recall its identity and actually verify the signatures. Matched to the
// propagation node's cadence: a client needs to hear this occasionally, and
// every announce is airtime off a shared half-duplex channel.
#ifndef RRC_BRIDGE_ANNOUNCE_MS
#define RRC_BRIDGE_ANNOUNCE_MS 1800000     // 30 minutes
#endif
#ifndef RRC_BRIDGE_FIRST_ANNOUNCE_MS
#define RRC_BRIDGE_FIRST_ANNOUNCE_MS 45000 // shortly after boot, before any traffic
#endif

// Provisioned configuration (RRC namespace).
extern bool rrc_bridge_enabled;
// Comma-separated room names, e.g. "general,command". Parsed into the bridged
// set at begin(); a leading '#' is optional and ignored, as everywhere in RRC.
extern char rrc_bridge_rooms[128];
// Lines of catch-up sent to a member joining a bridged room for the first
// time. 0 disables it; the effective value is clamped to
// RRC_BRIDGE_HISTORY_MAX.
extern uint8_t rrc_bridge_history;

void rrc_bridge_begin(const RNS::Identity& hub_identity);

// True if this room's traffic should be mirrored to LXMF. Takes an
// already-normalized room name.
bool rrc_bridge_bridged(const std::string& room);

// Record a member of a bridged room, so they can be addressed once they are
// gone. Safe to call on every join; it updates rather than duplicates.
void rrc_bridge_remember(const std::string& room, const RNS::Identity& member);

// Queue one accepted room message for delivery to everyone in the roster who
// is not in `present` -- the members holding a session at the moment it was
// said. Cheap: resolves recipients and copies text, and does no cryptography.
//
// Presence is passed in rather than looked up so the bridge stays independent
// of hub state; the caller already has the member list it just fanned out to.
void rrc_bridge_publish(const std::string& room, const std::string& nickname,
                        const RRC::IdentityHash& sender,
                        const RRC::MessageId& message_id, uint64_t timestamp_ms,
                        const std::string& text,
                        const std::vector<RRC::IdentityHash>& present);

// Compose and store at most one message per call, from the main loop.
void rrc_bridge_loop();

size_t rrc_bridge_room_count();
size_t rrc_bridge_history_depth();
size_t rrc_bridge_member_count();
size_t rrc_bridge_queue_depth();
uint32_t rrc_bridge_delivered_count();
uint32_t rrc_bridge_dropped_count();

#endif // RRC_HUB && LXMF_PROPAGATION_NODE
