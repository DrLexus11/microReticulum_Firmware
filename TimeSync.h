// Pull UTC from a peer that has better time than we do.
//
// Step 3 of docs/TimePropagation.md, in its unsigned form. Until now a node
// only ever got time because a human ran tools/set_node_time.py at it, which
// does not survive a deployment of apartments. A node that knows a peer worth
// asking can now fix its own clock unattended.
//
// The safety rules are unchanged and still live in the library: never
// backwards, bounded forward step, provenance recorded. This adds one more,
// which is what stops two nodes handing time back and forth forever: adopt
// only from a strictly better stratum.
//
// Deliberately not yet signed. On a private IFAC network membership is already
// the admission control, and a peer that reaches us has passed it. That makes a
// rogue member able to skew clocks within the step bounds, which is the reason
// TimePropagation.md specifies signed assertions as the hardening step -- this
// is the shape, not the finished trust model.
#pragma once

#if defined(HAS_RNS)

#include <MsgPack.h>

#ifndef TIME_SYNC_POLL_MS
#define TIME_SYNC_POLL_MS 60000UL
#endif
// How stale a verified clock may get before we go looking again. Well under the
// interval at which a restored clock's error becomes interesting, and cheap:
// one small request against one peer.
#ifndef TIME_SYNC_REVERIFY_MS
#define TIME_SYNC_REVERIFY_MS 3600000ULL
#endif
#ifndef TIME_SYNC_LINK_TIMEOUT_MS
#define TIME_SYNC_LINK_TIMEOUT_MS 45000UL
#endif
// A peer cannot hand us more than a week in one step, matching the NTP path.
#ifndef TIME_SYNC_MAX_STEP_MS
#define TIME_SYNC_MAX_STEP_MS 604800000ULL
#endif

// Destination hash of the node to ask. Empty disables the whole mechanism.
inline RNS::Bytes time_sync_peer_hash;

// Identity hashes permitted to assert time.
//
// Empty means "trust any peer that reaches us", which is the IFAC-membership
// model: passing the network's admission control is what belongs-here means,
// and it is the v1 behaviour. Once this list is non-empty a reply must carry a
// signature from one of these identities over the nonce we sent, and an
// unsigned or wrongly-signed answer is refused. Hardening is therefore opt-in
// and turning it on cannot silently break a node that was working.
inline std::vector<RNS::Bytes> time_sync_authorities;

struct TimeSyncState {
  RNS::Link link = {RNS::Type::NONE};
  uint64_t nonce = 0;
  RNS::Bytes peer_identity_hash;
  bool active = false;
  // Callbacks run inside the link and resource machinery, so they must not
  // tear the link down -- destroying the object the callback is executing
  // under panics the core (Guru Meditation, unhandled debug exception,
  // observed immediately after a successful adoption). They record the outcome
  // and the loop cleans up on its next pass instead.
  bool finished = false;
  const char* finish_reason = nullptr;
  uint32_t started = 0;
  uint32_t next_poll = 0;
  uint32_t attempts = 0;
  uint32_t adoptions = 0;
  uint32_t refusals = 0;
};

inline TimeSyncState& time_sync_state() {
  static TimeSyncState state;
  return state;
}

// Called from callbacks: record only, never touch the link.
inline void time_sync_finish(const char* why) {
  TimeSyncState& st = time_sync_state();
  st.finished = true;
  st.finish_reason = why;
}

// Called from the loop, where destroying the link is safe.
inline void time_sync_release() {
  TimeSyncState& st = time_sync_state();
  if (st.link) st.link.teardown();
  st.link = RNS::Link(RNS::Type::NONE);
  st.active = false;
  st.finished = false;
  if (st.finish_reason != nullptr) printf("[timesync] %s\n", st.finish_reason);
  st.finish_reason = nullptr;
}

inline void time_sync_response(const RNS::Bytes& response) {
  using OS = RNS::Utilities::OS;
  TimeSyncState& st = time_sync_state();

  // The reply is a map; pull the fields we care about by name rather than by
  // position, so adding fields to the handler cannot break this.
  uint64_t unix_ms = 0;
  uint8_t peer_stratum = 0;
  bool peer_known = false;
  uint64_t echoed_nonce = 0;
  RNS::Bytes signature;
  {
    MsgPack::Unpacker u;
    u.feed(response.data(), response.size());
    if (!u.isMap()) { time_sync_finish("reply was not a map"); return; }
    const size_t entries = u.unpackMapSize();
    for (size_t i = 0; i < entries; ++i) {
      MsgPack::str_t key;
      u.deserialize(key);
      if (key == "unix_ms") u.deserialize(unix_ms);
      else if (key == "stratum") u.deserialize(peer_stratum);
      else if (key == "known") u.deserialize(peer_known);
      else if (key == "nonce") u.deserialize(echoed_nonce);
      else if (key == "sig") {
        MsgPack::bin_t<uint8_t> raw;
        u.deserialize(raw);
        signature = RNS::Bytes(raw.data(), raw.size());
      }
      else { MsgPack::object::nil_t ignored; u.deserialize(ignored); }
    }
  }

  if (!peer_known || unix_ms == 0) {
    st.refusals++;
    time_sync_finish("peer has no time of its own");
    return;
  }

  if (!time_sync_authorities.empty()) {
    bool listed = false;
    for (const RNS::Bytes& allowed : time_sync_authorities) {
      if (allowed == st.peer_identity_hash) { listed = true; break; }
    }
    if (!listed) {
      st.refusals++;
      time_sync_finish("peer is not a time authority");
      return;
    }
    // The nonce is what makes this proof of current time rather than a
    // recording. Without it a replayed assertion is indistinguishable from a
    // fresh one to a node that has no clock of its own -- which is precisely
    // the node that needs the answer.
    if (echoed_nonce != st.nonce || signature.size() == 0) {
      st.refusals++;
      time_sync_finish("authority did not sign our nonce");
      return;
    }
    RNS::Identity peer_identity = RNS::Identity::recall(time_sync_peer_hash);
    uint8_t message[17];
    for (uint8_t i = 0; i < 8; ++i) message[i]     = (uint8_t)(echoed_nonce >> (56 - 8 * i));
    for (uint8_t i = 0; i < 8; ++i) message[8 + i] = (uint8_t)(unix_ms >> (56 - 8 * i));
    message[16] = peer_stratum;
    if (!peer_identity ||
        !peer_identity.validate(signature, RNS::Bytes(message, sizeof(message)))) {
      st.refusals++;
      time_sync_finish("time assertion failed signature check");
      return;
    }
  }

  // Strictly better, or this is a loop waiting to happen: two nodes at the same
  // stratum would each keep adopting from the other.
  const uint8_t mine = OS::wall_time_stratum();
  if (OS::wall_time_known() && mine != 0 && peer_stratum >= mine) {
    st.refusals++;
    time_sync_finish("peer is no closer to a reference than we are");
    return;
  }

  // A restored clock is a lower bound and cannot account for time spent
  // powered off, so it may be advanced past the usual step limit -- exactly as
  // the NTP path does.
  const uint64_t max_step =
      OS::wall_time_source() == OS::WallTimeSource::PERSISTED
      ? UINT64_MAX : TIME_SYNC_MAX_STEP_MS;
  const auto result = RNS::Reticulum::adopt_wall_time(
      unix_ms, OS::WallTimeSource::AUTHENTICATED_CLIENT, max_step,
      (uint8_t)(peer_stratum + 1));

  if (result == OS::WallTimeResult::ACCEPTED) {
    st.adoptions++;
    printf("[timesync] adopted UTC %llu ms from peer at stratum %u (now %u)\n",
           (unsigned long long)unix_ms, (unsigned)peer_stratum,
           (unsigned)OS::wall_time_stratum());
    time_sync_finish(nullptr);
    return;
  }
  // Backwards is the ordinary case once our own clock is good: the peer agreed
  // closely enough that there was nothing to apply, so record the agreement
  // rather than counting it as a failure.
  if (result == OS::WallTimeResult::BACKWARDS) {
    OS::note_wall_time_verified();
    time_sync_finish(nullptr);
    return;
  }
  st.refusals++;
  time_sync_finish("peer time rejected by the safety rules");
}

inline void time_sync_link_established(RNS::Link& link) {
  TimeSyncState& st = time_sync_state();
  if (!st.active) return;
  // Identify even though reading is public: a node that says who it is can be
  // held to it, and the peer logs the exchange.
  link.identify(RNS::Transport::identity());
  // [0, nonce] -- zero means "tell me your time, do not take mine", and the
  // nonce asks for a signed assertion that cannot be a replay.
  MsgPack::Packer packer;
  packer.serialize(MsgPack::arr_size_t(2));
  packer.serialize((uint64_t)0);
  packer.serialize(st.nonce);
  link.request(RNS::Bytes("/time"),
               RNS::Bytes(packer.data(), packer.size()),
               [](const RNS::RequestReceipt& receipt) {
                 time_sync_response(receipt.get_response());
               },
               [](const RNS::RequestReceipt&) {
                 time_sync_state().refusals++;
                 time_sync_finish("request failed");
               });
}

inline void time_sync_link_closed(RNS::Link& link) {
  TimeSyncState& st = time_sync_state();
  // The peer closes the link once it has answered, so this fires on the happy
  // path too. Only report it as early if nothing has already recorded an
  // outcome, or a successful sync gets logged as a failure.
  if (st.active && !st.finished) time_sync_finish("link closed before answering");
}

inline bool time_sync_wants_time() {
  using OS = RNS::Utilities::OS;
  if (!OS::wall_time_known()) return true;
  // A restored clock has never been confirmed since the reboot and is only a
  // lower bound, so it always wants a source.
  if (OS::wall_time_source() == OS::WallTimeSource::PERSISTED) return true;
  const uint64_t now = OS::monotonic_time_millis();
  const uint64_t verified = OS::wall_time_verified_at();
  // A restored record's stamp comes from a previous boot and can sit ahead of
  // the current monotonic clock, because the logical time offset is persisted
  // on a slower cadence than the wall-time record. Treat that as "unknown age"
  // and ask, rather than subtracting into an enormous positive number.
  if (verified > now) return true;
  return (now - verified) >= TIME_SYNC_REVERIFY_MS;
}

inline void time_sync_loop() {
  TimeSyncState& st = time_sync_state();
  if (time_sync_peer_hash.size() == 0) return;

  const uint32_t now = millis();
  if (st.finished) { time_sync_release(); return; }
  if (st.active) {
    if ((uint32_t)(now - st.started) > TIME_SYNC_LINK_TIMEOUT_MS) {
      st.refusals++;
      st.finish_reason = "timed out waiting for the peer";
      time_sync_release();
    }
    return;
  }
  if (st.next_poll != 0 && (uint32_t)(now - st.next_poll) > (uint32_t)1 << 31) return;
  st.next_poll = now + TIME_SYNC_POLL_MS;

  if (!time_sync_wants_time()) return;
  if (!RNS::Transport::has_path(time_sync_peer_hash)) {
    RNS::Transport::request_path(time_sync_peer_hash);
    return;   // try again next poll, once a path exists
  }
  RNS::Identity peer_identity = RNS::Identity::recall(time_sync_peer_hash);
  if (!peer_identity) return;

  RNS::Destination peer_mgmt(peer_identity, RNS::Type::Destination::OUT,
                             RNS::Type::Destination::SINGLE,
                             RNS::Type::Transport::APP_NAME, "remote.management");
  st.nonce = ((uint64_t)esp_random() << 32) | (uint64_t)esp_random();
  if (st.nonce == 0) st.nonce = 1;   // zero means "unsigned" on the wire
  st.peer_identity_hash = peer_identity.hash();
  st.active = true;
  st.started = now;
  st.attempts++;
  st.link = RNS::Link(peer_mgmt, time_sync_link_established,
                      time_sync_link_closed);
}

#else
inline void time_sync_loop() {}
#endif
