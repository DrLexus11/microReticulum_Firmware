// Outbound propagation-node peering: discover peers, offer them what we hold.
//
// This is the half of the sync that makes two RADs converge. LXMFPropagation.h
// answers /offer; nothing there ever *makes* an offer, so before this a message
// stored on one RAD stayed invisible to a client syncing with the other, and
// which node a client happened to pick silently decided what it received.
//
// Shape, from LXMRouter.py:
//
//   1. discover a peer from its lxmf.propagation announce
//   2. link to it and call /offer with the transient ids we hold
//   3. it replies with the subset it wants (or false)
//   4. send those messages as a Resource
//
// AIRTIME IS THE CONSTRAINT, not correctness. Every step above crosses LoRa on
// a shared, duty-cycled channel that also carries the traffic this node exists
// to move. Everything here is therefore rate-limited, bounded and lazy: we sync
// rarely, offer a bounded slice, and do nothing at all when we hold nothing new.
// Getting this wrong degrades the mesh for everyone on it, which is worse than
// the problem being solved.

#pragma once

#include "LXMFPropagation.h"

#if defined(HAS_RNS) && defined(LXMF_PROPAGATION_NODE)

// How often to attempt a sync with any one peer. Deliberately long: a
// propagation store is a backstop for offline clients, not a live feed, and
// LoRa airtime spent here is airtime taken from delivery.
#ifndef LXMF_PEER_SYNC_INTERVAL_MS
#define LXMF_PEER_SYNC_INTERVAL_MS 1800000   // 30 minutes
#endif

// Wait this long after boot before the first sync, so announces have had a
// chance to arrive and the node is not syncing while it is still settling.
#ifndef LXMF_PEER_SYNC_FIRST_MS
#define LXMF_PEER_SYNC_FIRST_MS 300000       // 5 minutes
#endif

// Hop ceiling for a peer, mirroring lxmd's autopeer_maxdepth. A distant peer is
// reachable but every message crosses every hop between, so peering with one is
// airtime spent on relays that gain us nothing.
#ifndef LXMF_PEER_MAX_DEPTH
#define LXMF_PEER_MAX_DEPTH 3
#endif

// Peers to remember. Small on purpose: this is a two-RAD deployment feature,
// and an unbounded table is something any stranger could grow by announcing.
#ifndef LXMF_PEER_MAX_PEERS
#define LXMF_PEER_MAX_PEERS 4
#endif

// Transient ids to put in one /offer. 32 bytes each, so this is the request's
// size on the air: 16 ids is about 512 bytes, which crosses LoRa in one sane
// transfer. Offering the whole store would be a multi-kilobyte request before
// a single message had moved.
#ifndef LXMF_PEER_OFFER_BATCH
#define LXMF_PEER_OFFER_BATCH 16
#endif

// Give up on a sync that has not concluded. Long, because LoRa is slow and a
// link plus a request plus a resource is several round trips.
#ifndef LXMF_PEER_SYNC_TIMEOUT_MS
#define LXMF_PEER_SYNC_TIMEOUT_MS 180000     // 3 minutes
#endif

struct LXMFPeerRecord {
	RNS::Bytes destination_hash;
	uint32_t   last_attempt_ms = 0;
	bool       ever_synced     = false;
};

// Diagnostics. Announce-handler dispatch in this port happens only inside the
// "should_add" branch of Transport::inbound -- a repeat announce from a path we
// already know never reaches a handler at all, and neither does a PATH_RESPONSE.
// Counting both filtered and unfiltered arrivals is the only way to tell "no
// peer announced" from "announces arrive but never reach us".
inline uint32_t& lxmf_peer_announces_filtered() { static uint32_t n = 0; return n; }
inline uint32_t& lxmf_peer_announces_any()      { static uint32_t n = 0; return n; }

inline std::vector<LXMFPeerRecord>& lxmf_peers() {
	static std::vector<LXMFPeerRecord> peers;
	return peers;
}

// One sync at a time. Two concurrent links to different peers would double the
// airtime for no gain, and the store is small enough that serialising costs
// nothing worth measuring.
struct LXMFPeerSyncState {
	bool        active   = false;
	uint32_t    started  = 0;
	RNS::Bytes  peer;
	RNS::Link   link     = {RNS::Type::NONE};
	bool        offered  = false;
};

inline LXMFPeerSyncState& lxmf_sync_state() {
	static LXMFPeerSyncState state;
	return state;
}

inline void lxmf_peer_sync_finish(const char* why) {
	LXMFPeerSyncState& st = lxmf_sync_state();
	if (!st.active) return;

	// Clear the state BEFORE tearing the link down, and take a local copy of
	// the link to tear down afterwards.
	//
	// Link::teardown() calls link_closed() synchronously, which calls our
	// closed callback, which calls this function again. Clearing st.active
	// afterwards left the guard above still true on re-entry, so this recursed
	// until the loopTask stack canary fired:
	//
	//   lxmf_peer_sync_finish -> Link::teardown -> Link::link_closed
	//     -> lxmf_peer_link_closed -> lxmf_peer_sync_finish -> ...
	//
	// Measured on hardware as a PANIC roughly 300s after boot, once a peer
	// existed for the sync path to run against at all.
	st.active  = false;
	st.offered = false;
	const RNS::Bytes peer = st.peer;
	RNS::Link link = st.link;
	st.link = {RNS::Type::NONE};
	st.peer = RNS::Bytes();

	printf("[lxmf-peer] sync with <%s> ended: %s\n",
	       peer.toHex().substr(0, 16).c_str(), why);
	if (link) {
		try { link.teardown(); } catch (...) {}
	}
}

// --- discovery --------------------------------------------------------------

class LXMFPeerAnnounceHandler : public RNS::AnnounceHandler {
public:
	LXMFPeerAnnounceHandler() : RNS::AnnounceHandler("lxmf.propagation") {}

	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		lxmf_peer_announces_filtered()++;
		// Never peer with ourselves. Our own announce comes back over any
		// interface that loops, and a node syncing with itself would offer its
		// whole store to itself once every interval, forever.
		extern RNS::Destination lxmf_propagation_destination;
		if (lxmf_propagation_destination &&
		    destination_hash == lxmf_propagation_destination.hash()) return;

		const uint8_t hops = RNS::Transport::hops_to(destination_hash);
		if (hops > LXMF_PEER_MAX_DEPTH) {
			printf("[lxmf-peer] ignoring <%s>: %u hops, over the depth limit of %d\n",
			       destination_hash.toHex().substr(0, 16).c_str(),
			       (unsigned)hops, LXMF_PEER_MAX_DEPTH);
			return;
		}

		for (auto& p : lxmf_peers()) {
			if (p.destination_hash == destination_hash) return;   // already known
		}
		if (lxmf_peers().size() >= LXMF_PEER_MAX_PEERS) {
			printf("[lxmf-peer] peer table full, ignoring <%s>\n",
			       destination_hash.toHex().substr(0, 16).c_str());
			return;
		}

		LXMFPeerRecord record;
		record.destination_hash = destination_hash;
		// Not zero: a freshly discovered peer waits out the normal interval
		// rather than being synced with the moment it is heard, which would
		// make a rebooting neighbour trigger a sync every time it announces.
		record.last_attempt_ms = millis();
		lxmf_peers().push_back(record);
		printf("[lxmf-peer] discovered peer <%s> at %u hop(s)\n",
		       destination_hash.toHex().substr(0, 16).c_str(), (unsigned)hops);
	}
};

// Counts every announce that reaches a handler, whatever its aspect. If this
// stays at zero while peers are demonstrably announcing, the problem is
// dispatch, not our filter.
class LXMFAnyAnnounceCounter : public RNS::AnnounceHandler {
public:
	LXMFAnyAnnounceCounter() : RNS::AnnounceHandler(nullptr) {}
	void received_announce(const RNS::Bytes& destination_hash,
	                       const RNS::Identity& announced_identity,
	                       const RNS::Bytes& app_data) override {
		lxmf_peer_announces_any()++;
	}
};

// A statically configured peer, as 32 hex characters, or empty.
//
// Announce-based discovery is not dependable in this port: Transport dispatches
// announce handlers only when a path is newly learned, and skips them entirely
// for PATH_RESPONSE. A node that learns a peer's path by requesting it -- which
// is exactly what happens when it first delivers a message there -- consumes the
// only discovery opportunity it will get, silently. Measured on hardware: one
// announce of any aspect reached a handler in 3.5 minutes and none of them was a
// propagation announce.
//
// For the deployment this feature exists for -- two RADs, one site -- the peer
// is known in advance. lxmd has static peers for the same reason. Discovery by
// announce is kept as an opportunistic extra, not the mechanism.
extern char lxmf_static_peer[33];

inline void lxmf_peer_add_static() {
	if (lxmf_static_peer[0] == 0) return;
	RNS::Bytes hash;
	hash.assignHex(lxmf_static_peer);
	if (hash.size() != LXMF_DESTINATION_LEN) {
		printf("[lxmf-peer] static peer \"%s\" is not %d hex bytes, ignoring\n",
		       lxmf_static_peer, LXMF_DESTINATION_LEN);
		return;
	}
	for (auto& p : lxmf_peers()) if (p.destination_hash == hash) return;
	LXMFPeerRecord record;
	record.destination_hash = hash;
	// Zero, not millis(): a configured peer is synced with at the first
	// opportunity rather than after a full interval of doing nothing.
	record.last_attempt_ms = 0;
	lxmf_peers().push_back(record);
	printf("[lxmf-peer] static peer <%s>\n", hash.toHex().c_str());
}

inline void lxmf_peer_sync_begin() {
	static RNS::HAnnounceHandler handler(new LXMFPeerAnnounceHandler());
	RNS::Transport::register_announce_handler(handler);
	static RNS::HAnnounceHandler counter(new LXMFAnyAnnounceCounter());
	RNS::Transport::register_announce_handler(counter);
	lxmf_peer_add_static();
	printf("[lxmf-peer] listening for propagation-node announces\n");
}

// --- the sync itself --------------------------------------------------------

// The peer told us which of our ids it wants. Send exactly those, as a Resource,
// keeping the total inside the sync limit we advertise -- the peer's own
// resource guard will refuse anything larger, so exceeding it wastes the
// transfer rather than delivering more.
inline void lxmf_peer_offer_response(const RNS::Bytes& response) {
	LXMFPeerSyncState& st = lxmf_sync_state();
	if (!st.active) return;

	MsgPack::Unpacker unpacker;
	unpacker.feed(response.data(), response.size());

	if (unpacker.isBool()) {
		// false is "none of these"; true is "all of them".
		const bool all = unpacker.unpackBool();
		if (!all) { lxmf_peer_sync_finish("peer wanted nothing"); return; }
	}
	else if (!unpacker.isArray()) {
		lxmf_peer_sync_finish("unparseable /offer response");
		return;
	}

	std::vector<RNS::Bytes> send;
	size_t total = 0;
	if (unpacker.isArray()) {
		const size_t count = unpacker.unpackArraySize();
		for (size_t i = 0; i < count; i++) {
			if (!unpacker.isBin()) break;
			MsgPack::bin_t<uint8_t> id_bin = unpacker.unpackBinary<uint8_t>();
			RNS::Bytes id(id_bin.data(), id_bin.size());
			for (const auto& e : lxmf_store_index) {
				if (e.transient_id != id) continue;
				if (total + e.size > LXMF_PN_SYNC_LIMIT_BYTES) break;
				RNS::Bytes blob;
				const std::string path = lxmf_entry_path(e.transient_id, e.received,
				                                         e.from_peer);
				if (RNS::Utilities::OS::read_file(path.c_str(), blob) != (int)e.size) break;
				send.push_back(blob);
				total += e.size;
				break;
			}
		}
	}

	if (send.empty()) { lxmf_peer_sync_finish("nothing left to send"); return; }

	// Same container shape a client pushes to us: [timebase, [messages]].
	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(2));
	packer.serialize((uint64_t)RNS::Utilities::OS::time());
	packer.serialize(MsgPack::arr_size_t(send.size()));
	for (const RNS::Bytes& m : send) {
		packer.serialize(MsgPack::bin_t<uint8_t>(m.data(), m.data() + m.size()));
	}
	RNS::Bytes container(packer.data(), packer.size());

	printf("[lxmf-peer] sending %u message(s), %u bytes to <%s>\n",
	       (unsigned)send.size(), (unsigned)container.size(),
	       st.peer.toHex().substr(0, 16).c_str());
	try {
		RNS::Resource resource(container, st.link);
		(void)resource;
	}
	catch (const std::exception& e) {
		printf("[lxmf-peer] resource failed: %s\n", e.what());
	}
	lxmf_peer_sync_finish("messages handed to the resource layer");
}

inline void lxmf_peer_request_response(const RNS::RequestReceipt& receipt) {
	lxmf_peer_offer_response(receipt.get_response());
}

inline void lxmf_peer_request_failed(const RNS::RequestReceipt& receipt) {
	lxmf_peer_sync_finish("/offer request failed");
}

inline void lxmf_peer_link_established(RNS::Link& link) {
	LXMFPeerSyncState& st = lxmf_sync_state();
	if (!st.active || st.offered) return;

	// Identify: the peer's handler refuses an unidentified link, exactly as
	// ours does. Peering is between nodes, and a node that will not say who it
	// is has no business handing us a backlog.
	link.identify(RNS::Transport::identity());

	std::vector<RNS::Bytes> ids;
	for (const auto& e : lxmf_store_index) {
		if (ids.size() >= LXMF_PEER_OFFER_BATCH) break;
		ids.push_back(e.transient_id);
	}
	if (ids.empty()) { lxmf_peer_sync_finish("nothing to offer"); return; }

	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(2));
	// Peering key, sent empty. We do not require one inbound and do not claim
	// one outbound; admission on both sides is the store share, not a shared
	// secret. An empty bin rather than nil, because lxmd's default posture is
	// auth_required = no and a bin is the type its own peers send.
	packer.serialize(MsgPack::bin_t<uint8_t>{});
	packer.serialize(MsgPack::arr_size_t(ids.size()));
	for (const RNS::Bytes& id : ids) {
		packer.serialize(MsgPack::bin_t<uint8_t>(id.data(), id.data() + id.size()));
	}

	printf("[lxmf-peer] offering %u id(s) to <%s>\n",
	       (unsigned)ids.size(), st.peer.toHex().substr(0, 16).c_str());
	st.offered = true;
	link.request(RNS::Bytes(LXMF_OFFER_PATH),
	             RNS::Bytes(packer.data(), packer.size()),
	             lxmf_peer_request_response, lxmf_peer_request_failed);
}

inline void lxmf_peer_link_closed(RNS::Link& link) {
	lxmf_peer_sync_finish("link closed");
}

// Called from the main loop. Does nothing at all in the common case.
inline void lxmf_peer_sync_watch() {
	static uint32_t last_tick = 0;
	const uint32_t now = millis();

	LXMFPeerSyncState& st = lxmf_sync_state();
	if (st.active) {
		if (now - st.started > LXMF_PEER_SYNC_TIMEOUT_MS)
			lxmf_peer_sync_finish("timed out");
		return;
	}

	static uint32_t last_status = 0;
	if (now - last_status >= 60000) {
		last_status = now;
		printf("[lxmf-peer] status: %u peer(s), %u store, announces %u/%u (prop/any)\n",
		       (unsigned)lxmf_peers().size(), (unsigned)lxmf_store_index.size(),
		       (unsigned)lxmf_peer_announces_filtered(),
		       (unsigned)lxmf_peer_announces_any());
	}

	if (now < LXMF_PEER_SYNC_FIRST_MS) return;
	if (now - last_tick < 10000) return;      // do not scan the table every loop
	last_tick = now;

	// Nothing to offer, nothing to do. This is the common case and it must cost
	// no airtime whatsoever.
	if (lxmf_store_index.empty()) return;

	for (auto& p : lxmf_peers()) {
		if (now - p.last_attempt_ms < LXMF_PEER_SYNC_INTERVAL_MS) continue;
		p.last_attempt_ms = now;

		if (!RNS::Transport::has_path(p.destination_hash)) {
			printf("[lxmf-peer] no path to <%s>, requesting one\n",
			       p.destination_hash.toHex().substr(0, 16).c_str());
			RNS::Transport::request_path(p.destination_hash);
			return;   // try again next interval, once the path has had time
		}

		RNS::Identity peer_identity = RNS::Identity::recall(p.destination_hash);
		if (!peer_identity) {
			printf("[lxmf-peer] cannot recall identity for <%s>\n",
			       p.destination_hash.toHex().substr(0, 16).c_str());
			return;
		}

		RNS::Destination peer_destination(
			peer_identity, RNS::Type::Destination::OUT,
			RNS::Type::Destination::SINGLE, LXMF_APP_NAME, LXMF_PN_ASPECT);

		st.active  = true;
		st.offered = false;
		st.started = now;
		st.peer    = p.destination_hash;
		printf("[lxmf-peer] syncing with <%s>\n",
		       p.destination_hash.toHex().substr(0, 16).c_str());
		st.link = RNS::Link(peer_destination, lxmf_peer_link_established,
		                    lxmf_peer_link_closed);
		return;   // one at a time
	}
}

#endif // HAS_RNS && LXMF_PROPAGATION_NODE
