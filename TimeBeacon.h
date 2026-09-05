// Periodic signed time assertions, relayed by the mesh for free.
//
// Step 4 of docs/TimePropagation.md. Step 3 (TimeSync.h) fixed the console
// problem -- a node can now get UTC without a human at a CLI -- but it still
// needs to reach a specific peer and open a Link. OZD-01 is the counterexample
// sitting on the bench: unreachable, no path, no Link possible, and its OLED
// shows an uptime counter instead of a clock because nothing can ever tell it
// what time it is. A node that can only listen has to be able to learn the
// time by listening.
//
// So: an authority announces a destination whose app data IS the assertion.
// Reticulum's announce machinery then does the distribution -- flooding,
// hop counting, deduplication by packet hash -- and every relay in between is
// untrusted by construction, because the assertion carries the authority's own
// signature and a relay can drop or delay it but cannot forge it.
//
// What this deliberately does not solve: a node with no clock at all cannot
// judge whether a signed assertion is fresh or a recording. Only the nonce
// challenge in TimeSync.h can prove that, and it needs a Link. A listen-only
// node therefore bootstraps to "probably right" and is honest about it --
// the provenance it records is SIGNED_BEACON, not NTP -- while anti-rollback
// stops a replayed assertion from ever dragging it backwards.
#pragma once

#if defined(HAS_RNS)

#include <MsgPack.h>

// The aspect an authority announces under. The filter an announce handler is
// matched against is the full expanded name, app name included.
#define TIME_BEACON_ASPECT "time.assertion"
#define TIME_BEACON_FILTER "rnstransport.time.assertion"

// Cadence. An assertion is ~110 bytes on top of the announce, so at the
// default this is a few hundred bytes an hour from each authority -- free even
// inside a 1% duty cycle, and there are only ever a handful of authorities.
#ifndef TIME_BEACON_INTERVAL_MS
#define TIME_BEACON_INTERVAL_MS 1800000UL
#endif
// Rolled per emission so a fleet of authorities restored to mains power
// together does not beacon in lockstep forever.
#ifndef TIME_BEACON_JITTER_MS
#define TIME_BEACON_JITTER_MS 120000UL
#endif
// How long an assertion may be treated as fresh by a receiver that has a clock
// of its own to judge it with.
#ifndef TIME_BEACON_VALID_FOR_S
#define TIME_BEACON_VALID_FOR_S 7200UL
#endif
// A beacon may not step a receiver's clock further than this, matching the
// bound on every other path that can set the clock.
#ifndef TIME_BEACON_MAX_STEP_MS
#define TIME_BEACON_MAX_STEP_MS 604800000ULL
#endif

// The bytes an authority signs. The leading string is domain separation: the
// solicited reply in Transport::remote_time_handler signs a different 17-byte
// layout, and without a distinct domain a signature captured from one exchange
// could be presented as the other.
#define TIME_BEACON_DOMAIN "urtn-time-beacon-v1"
#define TIME_BEACON_DOMAIN_LEN 19
#define TIME_BEACON_SIGNED_LEN (TIME_BEACON_DOMAIN_LEN + 8 + 4 + 1 + 1)

// Set by provisioning: whether this node originates assertions at all.
inline bool time_beacon_enabled = false;
inline uint32_t time_beacon_interval_s = TIME_BEACON_INTERVAL_MS / 1000UL;

// The destination an authority announces. Only created when enabled.
inline RNS::Destination time_beacon_destination{RNS::Type::NONE};

// Trust anchors are shared with TimeSync: an identity permitted to answer a
// nonce challenge is exactly an identity permitted to beacon. Keeping one list
// means an operator cannot provision half a trust model.
extern std::vector<RNS::Bytes> time_sync_authorities;

struct TimeBeaconStats {
  uint32_t emitted = 0;
  uint32_t heard = 0;          // announces that reached our handler
  uint32_t verified = 0;       // signature checked out against an authority
  uint32_t adopted = 0;
  uint32_t refused_unlisted = 0;
  uint32_t refused_signature = 0;
  uint32_t refused_stale = 0;  // older than one we already accepted, or expired
  uint32_t refused_rules = 0;  // the library's safety rules said no
  uint64_t highest_asserted_ms = 0;
  uint32_t last_emit = 0;
  uint32_t emit_jitter = 0;
  bool armed = false;
};

inline TimeBeaconStats& time_beacon_stats() {
  static TimeBeaconStats stats;
  return stats;
}

// Lay out the signed bytes identically on both sides. Big-endian throughout,
// fixed width, no msgpack: a canonicalisation disagreement between packer
// versions would produce signatures that can never verify, and the failure
// would look exactly like an attack.
inline void time_beacon_signed_bytes(uint8_t out[TIME_BEACON_SIGNED_LEN],
                                     uint64_t unix_ms, uint32_t valid_for_s,
                                     uint8_t stratum, uint8_t source) {
  memcpy(out, TIME_BEACON_DOMAIN, TIME_BEACON_DOMAIN_LEN);
  size_t at = TIME_BEACON_DOMAIN_LEN;
  for (uint8_t i = 0; i < 8; ++i) out[at++] = (uint8_t)(unix_ms >> (56 - 8 * i));
  for (uint8_t i = 0; i < 4; ++i) out[at++] = (uint8_t)(valid_for_s >> (24 - 8 * i));
  out[at++] = stratum;
  out[at++] = source;
}

// --- receiving --------------------------------------------------------------

inline void time_beacon_apply(const RNS::Identity& announced_identity,
                              const RNS::Bytes& app_data) {
  using OS = RNS::Utilities::OS;
  TimeBeaconStats& st = time_beacon_stats();
  st.heard++;

  uint8_t version = 0;
  uint64_t unix_ms = 0;
  uint32_t valid_for_s = 0;
  uint8_t stratum = 0;
  uint8_t source = 0;
  RNS::Bytes signature;
  {
    MsgPack::Unpacker u;
    u.feed(app_data.data(), app_data.size());
    if (!u.isMap()) return;   // somebody else's announce on our aspect
    const size_t entries = u.unpackMapSize();
    for (size_t i = 0; i < entries; ++i) {
      MsgPack::str_t key;
      u.deserialize(key);
      if (key == "v") u.deserialize(version);
      else if (key == "t") u.deserialize(unix_ms);
      else if (key == "f") u.deserialize(valid_for_s);
      else if (key == "s") u.deserialize(stratum);
      else if (key == "o") u.deserialize(source);
      else if (key == "g") {
        MsgPack::bin_t<uint8_t> raw;
        u.deserialize(raw);
        signature = RNS::Bytes(raw.data(), raw.size());
      }
      else { MsgPack::object::nil_t ignored; u.deserialize(ignored); }
    }
  }
  if (version != 1 || unix_ms == 0 || !signature) return;

  // Who signed it. An empty authority list means "any IFAC peer", which is the
  // v1 posture everywhere else in this firmware -- but not here. Originating
  // time is the one thing IFAC membership is explicitly not sufficient for
  // (docs/TimePropagation.md), because a beacon needs no reply and reaches
  // every node in the mesh. With no authorities provisioned we do not adopt.
  if (time_sync_authorities.empty()) {
    st.refused_unlisted++;
    return;
  }
  bool listed = false;
  for (const RNS::Bytes& allowed : time_sync_authorities) {
    if (allowed == announced_identity.hash()) { listed = true; break; }
  }
  if (!listed) { st.refused_unlisted++; return; }

  // Anti-rollback, and the reason a replay cannot hurt us. A captured
  // assertion is perfectly signed forever, so freshness cannot come from the
  // signature; it comes from refusing to go back over ground we have covered.
  if (unix_ms <= st.highest_asserted_ms) { st.refused_stale++; return; }

  // If we already have a clock we can also judge the assertion's own validity
  // window, which catches a relay that has been sitting on one.
  if (OS::wall_time_known() && valid_for_s > 0) {
    const uint64_t expires_at = unix_ms + (uint64_t)valid_for_s * 1000ULL;
    if (OS::wall_time_millis() > expires_at) { st.refused_stale++; return; }
  }

  uint8_t message[TIME_BEACON_SIGNED_LEN];
  time_beacon_signed_bytes(message, unix_ms, valid_for_s, stratum, source);
  if (!announced_identity.validate(signature,
                                   RNS::Bytes(message, sizeof(message)))) {
    st.refused_signature++;
    printf("[timebeacon] assertion from <%s> failed signature check\n",
           announced_identity.hash().toHex().substr(0, 16).c_str());
    return;
  }
  st.verified++;
  // Only ever record a value we have actually verified, or a forged assertion
  // could raise the rollback floor and lock out the real authority.
  st.highest_asserted_ms = unix_ms;

  // Strictly better, or two nodes at the same stratum keep adopting from each
  // other forever.
  const uint8_t mine = OS::wall_time_stratum();
  if (OS::wall_time_known() && mine != 0 && (uint8_t)(stratum + 1) >= mine &&
      OS::wall_time_source() != OS::WallTimeSource::PERSISTED) {
    // Not a refusal: the authority agrees with us, which is worth recording as
    // a successful check rather than counted as a failure.
    OS::note_wall_time_verified();
    return;
  }

  const uint64_t max_step = OS::wall_time_source() == OS::WallTimeSource::PERSISTED
      ? UINT64_MAX : TIME_BEACON_MAX_STEP_MS;
  const auto result = RNS::Reticulum::adopt_wall_time(
      unix_ms, OS::WallTimeSource::SIGNED_BEACON, max_step,
      (uint8_t)(stratum + 1));
  if (result == OS::WallTimeResult::ACCEPTED) {
    st.adopted++;
    printf("[timebeacon] adopted UTC %llu ms from authority <%s> at stratum %u "
           "(now %u)\n", (unsigned long long)unix_ms,
           announced_identity.hash().toHex().substr(0, 16).c_str(),
           (unsigned)stratum, (unsigned)OS::wall_time_stratum());
    return;
  }
  if (result == OS::WallTimeResult::BACKWARDS) {
    OS::note_wall_time_verified();
    return;
  }
  st.refused_rules++;
  printf("[timebeacon] assertion from <%s> rejected by the safety rules\n",
         announced_identity.hash().toHex().substr(0, 16).c_str());
}

class TimeBeaconAnnounceHandler : public RNS::AnnounceHandler {
public:
  TimeBeaconAnnounceHandler() : RNS::AnnounceHandler(TIME_BEACON_FILTER) {}
  void received_announce(const RNS::Bytes& destination_hash,
                         const RNS::Identity& announced_identity,
                         const RNS::Bytes& app_data) override {
    // Our own beacon comes back over any interface that loops.
    if (time_beacon_destination &&
        destination_hash == time_beacon_destination.hash()) return;
    if (!announced_identity || !app_data) return;
    time_beacon_apply(announced_identity, app_data);
  }
};

// --- originating ------------------------------------------------------------

inline RNS::Bytes time_beacon_assertion() {
  using OS = RNS::Utilities::OS;
  const uint64_t unix_ms = OS::wall_time_millis();
  const uint8_t stratum = OS::wall_time_stratum();
  const uint8_t source = (uint8_t)OS::wall_time_source();
  const uint32_t valid_for_s = TIME_BEACON_VALID_FOR_S;

  uint8_t message[TIME_BEACON_SIGNED_LEN];
  time_beacon_signed_bytes(message, unix_ms, valid_for_s, stratum, source);
  const RNS::Bytes signature =
      RNS::Transport::identity().sign(RNS::Bytes(message, sizeof(message)));

  MsgPack::Packer p;
  p.packMapSize(6);
  p.pack("v"); p.serialize((uint8_t)1);
  p.pack("t"); p.serialize(unix_ms);
  p.pack("f"); p.serialize(valid_for_s);
  p.pack("s"); p.serialize(stratum);
  p.pack("o"); p.serialize(source);
  p.pack("g"); p.serialize(MsgPack::bin_t<uint8_t>(
      signature.data(), signature.data() + signature.size()));
  return RNS::Bytes(p.data(), p.size());
}

inline void time_beacon_begin() {
  static RNS::HAnnounceHandler handler(new TimeBeaconAnnounceHandler());
  RNS::Transport::register_announce_handler(handler);

  if (time_beacon_enabled) {
    time_beacon_destination = RNS::Destination(
        RNS::Transport::identity(),
        RNS::Type::Destination::IN,
        RNS::Type::Destination::SINGLE,
        RNS::Type::Transport::APP_NAME,
        TIME_BEACON_ASPECT);
    printf("[timebeacon] originating as authority on <%s>\n",
           time_beacon_destination.hash().toHex().c_str());
  }
  printf("[timebeacon] listening for signed assertions (%u authority key(s))\n",
         (unsigned)time_sync_authorities.size());
}

inline void time_beacon_loop() {
  using OS = RNS::Utilities::OS;
  if (!time_beacon_enabled || !time_beacon_destination) return;
  TimeBeaconStats& st = time_beacon_stats();
  const uint32_t now = millis();
  if (!st.armed) {
    st.armed = true;
    st.last_emit = now;
    st.emit_jitter = (uint32_t)random(TIME_BEACON_JITTER_MS);
    return;
  }
  const uint32_t interval = (time_beacon_interval_s > 0)
      ? time_beacon_interval_s * 1000UL : TIME_BEACON_INTERVAL_MS;
  if ((uint32_t)(now - st.last_emit) < interval + st.emit_jitter) return;

  // Never assert a time we cannot stand behind. A node with an unknown or
  // merely restored clock has nothing to say: `persisted` is a lower bound,
  // not a measurement, and beaconing it would spread a plausible wrong answer
  // to nodes that have no way to check it.
  if (!OS::wall_time_known() ||
      OS::wall_time_source() == OS::WallTimeSource::PERSISTED ||
      OS::wall_time_stratum() == 0) {
    return;
  }

  st.last_emit = now;
  st.emit_jitter = (uint32_t)random(TIME_BEACON_JITTER_MS);
  st.emitted++;
  time_beacon_destination.announce(time_beacon_assertion());
  printf("[timebeacon] asserted UTC %llu ms at stratum %u (emission %u)\n",
         (unsigned long long)OS::wall_time_millis(),
         (unsigned)OS::wall_time_stratum(), (unsigned)st.emitted);
}

#else
inline void time_beacon_begin() {}
inline void time_beacon_loop() {}
#endif
