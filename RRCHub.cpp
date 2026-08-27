#if defined(RRC_HUB)

#include "RRCHub.h"

#include "RRCProtocol.h"
#include "RRCState.h"

#include <Arduino.h>
#include <cbor.h>
#include <esp_system.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr size_t MAX_SESSION_CAPACITY = 8;
constexpr size_t MAX_ROOMS = 16;
constexpr uint64_t ANNOUNCE_FIRST_MS = 15000;
constexpr char HUB_VERSION[] = "0.1";

struct Slot {
    bool used = false;
    RNS::Link link{RNS::Type::NONE};
    RNS::Bytes link_id;
    RRC::SessionKey key = 0;
    uint64_t last_ping_ms = 0;
};

RRC::StateLimits make_state_limits() {
    RRC::StateLimits limits;
    limits.max_sessions = rrc_hub_max_sessions;
    limits.max_rooms = MAX_ROOMS;
    limits.max_rooms_per_session = rrc_hub_max_rooms_per_session;
    limits.messages_per_minute = rrc_hub_rate_per_minute;
    limits.pong_timeout_ms =
        static_cast<uint64_t>(rrc_hub_pong_timeout_seconds) * 1000ULL;
    return limits;
}

RRC::HubState hub_state(make_state_limits());
std::array<Slot, MAX_SESSION_CAPACITY> slots{};
RNS::Destination destination{RNS::Type::NONE};
RRC::IdentityHash hub_identity_hash{};
uint64_t next_session_key = 1;
uint32_t rx_count = 0;
uint32_t tx_count = 0;
uint32_t accepted_count = 0;
uint32_t forwarded_count = 0;
uint32_t rejected_count = 0;
uint32_t rate_limited_count = 0;
uint32_t malformed_count = 0;
uint32_t identify_timeout_count = 0;
uint32_t hello_timeout_count = 0;
uint32_t pong_timeout_count = 0;
uint32_t last_millis = 0;
uint64_t millis_high = 0;
uint64_t last_announce_ms = 0;
bool announce_armed = false;
bool first_delayed_announce = false;

uint64_t monotonic_ms() {
    const uint32_t current = millis();
    if (current < last_millis) millis_high += (1ULL << 32);
    last_millis = current;
    return millis_high | current;
}

bool copy_identity_hash(const RNS::Identity& identity, RRC::IdentityHash& output) {
    if (!identity || identity.hash().size() != output.size()) return false;
    std::memcpy(output.data(), identity.hash().data(), output.size());
    return true;
}

void random_message_id(RRC::MessageId& output) {
    esp_fill_random(output.data(), output.size());
}

Slot* find_slot(const RNS::Bytes& link_id) {
    for (auto& slot : slots) {
        if (slot.used && slot.link_id == link_id) return &slot;
    }
    return nullptr;
}

Slot* find_slot(RRC::SessionKey key) {
    for (auto& slot : slots) {
        if (slot.used && slot.key == key) return &slot;
    }
    return nullptr;
}

Slot* free_slot() {
    for (auto& slot : slots) if (!slot.used) return &slot;
    return nullptr;
}

RRC::ValidationLimits validation_limits(Slot& slot) {
    RRC::ValidationLimits limits;
    limits.max_body_bytes = rrc_hub_max_body_bytes;
    const uint16_t mdu = slot.link ? slot.link.get_mdu() : 0;
    if (mdu > 0) limits.max_envelope_bytes = std::min<size_t>(mdu, RRC::MAX_ENVELOPE_BYTES);
    return limits;
}

RRC::Envelope base_envelope(uint8_t type) {
    RRC::Envelope envelope;
    envelope.type = type;
    random_message_id(envelope.message_id);
    envelope.timestamp_ms = 0; // No trusted wall clock is available on the node.
    envelope.source = hub_identity_hash;
    return envelope;
}

bool send_envelope(Slot& slot, const RRC::Envelope& envelope) {
    if (!slot.used || !slot.link || slot.link.status() != RNS::Type::Link::ACTIVE) return false;
    std::array<uint8_t, RRC::MAX_ENVELOPE_BYTES> buffer{};
    const RRC::ValidationLimits limits = validation_limits(slot);
    const RRC::Result result = RRC::encode(envelope, buffer.data(), buffer.size(), limits);
    if (!result) {
        ++rejected_count;
        return false;
    }
    try {
        RNS::Packet(slot.link, RNS::Bytes(buffer.data(), result.size)).send();
        ++tx_count;
        return true;
    } catch (...) {
        ++rejected_count;
        return false;
    }
}

void send_error(Slot& slot, const char* message) {
    RRC::Envelope error = base_envelope(RRC::T_ERROR);
    error.body = RRC::Body::text_value(message ? message : "request rejected");
    send_envelope(slot, error);
}

void reject(Slot& slot, const char* message) {
    ++rejected_count;
    send_error(slot, message);
}

std::vector<RRC::IdentityHash> member_hashes(const RRC::MemberKeys& keys) {
    std::vector<RRC::IdentityHash> hashes;
    hashes.reserve(keys.count);
    for (size_t i = 0; i < keys.count; ++i) {
        const auto identity = hub_state.identity(keys.values[i]);
        if (identity) hashes.push_back(*identity);
    }
    return hashes;
}

size_t fanout(const RRC::MemberKeys& members, const RRC::Envelope& envelope,
              std::optional<RRC::SessionKey> except = std::nullopt) {
    size_t sent = 0;
    for (size_t i = 0; i < members.count; ++i) {
        if (except && members.values[i] == *except) continue;
        Slot* target = find_slot(members.values[i]);
        if (target && send_envelope(*target, envelope)) ++sent;
    }
    return sent;
}

void send_welcome(Slot& slot) {
    RRC::WelcomeBody body;
    body.hub_name = rrc_hub_name;
    body.hub_version = HUB_VERSION;
    body.capabilities.action = true;
    body.capabilities.resource_envelope = false;
    RRC::HubLimits limits;
    limits.max_msg_body_bytes = rrc_hub_max_body_bytes;
    limits.max_rooms_per_session = rrc_hub_max_rooms_per_session;
    limits.rate_limit_msgs_per_minute = rrc_hub_rate_per_minute;
    body.limits = limits;

    RRC::Envelope welcome = base_envelope(RRC::T_WELCOME);
    welcome.body = RRC::Body::welcome_value(body);
    send_envelope(slot, welcome);
}

void handle_hello(Slot& slot, const RRC::Envelope& envelope, uint64_t now_ms) {
    if (!hub_state.identity(slot.key)) {
        reject(slot, "identify link before HELLO");
        return;
    }
    if (hub_state.welcomed(slot.key)) {
        send_welcome(slot); // HELLO is retried by clients until WELCOME arrives.
        return;
    }
    const RRC::StateError result = hub_state.hello(slot.key, envelope.nickname, now_ms);
    if (result != RRC::StateError::None) {
        reject(slot, RRC::state_error_string(result));
        return;
    }
    ++accepted_count;
    send_welcome(slot);
}

void handle_join(Slot& slot, const RRC::Envelope& envelope) {
    const std::string room = RRC::normalize_room(*envelope.room);
    const RRC::StateError result = hub_state.join(slot.key, room);
    if (result != RRC::StateError::None) {
        reject(slot, RRC::state_error_string(result));
        return;
    }
    ++accepted_count;

    const RRC::MemberKeys members = hub_state.members(room);
    RRC::Envelope joined = base_envelope(RRC::T_JOINED);
    joined.room = room;
    joined.body = RRC::Body::member_list(member_hashes(members));
    send_envelope(slot, joined);

    RRC::Envelope notification = base_envelope(RRC::T_JOINED);
    notification.room = room;
    const auto identity = hub_state.identity(slot.key);
    if (identity) notification.body = RRC::Body::member_list({*identity});
    notification.nickname = hub_state.nickname(slot.key);
    fanout(members, notification, slot.key);
}

void handle_part(Slot& slot, const RRC::Envelope& envelope) {
    const std::string room = RRC::normalize_room(*envelope.room);
    const RRC::MemberKeys before = hub_state.members(room);
    const auto identity = hub_state.identity(slot.key);
    const auto nickname = hub_state.nickname(slot.key);
    const RRC::StateError result = hub_state.part(slot.key, room);
    if (result != RRC::StateError::None) {
        reject(slot, RRC::state_error_string(result));
        return;
    }
    ++accepted_count;

    RRC::Envelope parted = base_envelope(RRC::T_PARTED);
    parted.room = room;
    if (identity) parted.body = RRC::Body::member_list({*identity});
    parted.nickname = nickname;
    send_envelope(slot, parted);
    fanout(before, parted, slot.key);
}

// --- Hub service commands -------------------------------------------------
//
// Stock clients issue these automatically and depend on the answers: NomadNet
// sends `/list` right after WELCOME to populate its channel list, and
// `/who <room>` right after JOIN to learn who is present. Without replies the
// room list stays empty and members render as bare hashes, which is exactly how
// the hub looked next to rrcd.
//
// The reply formats are NomadNet's parser contract, not our invention:
//   /list -> "Registered public rooms\n<room>\n<room>"  or
//            "No public rooms registered"
//   /who  -> "members in <room>: nick (hex12), <full-32-hex>, ..." or "(none)"
// A nicked member carries a 12-hex prefix; an un-nicked one the full 32 hex.
//
// Replies go only to the requester, as rrcd does, and are never fanned out.

bool body_starts_with(const RRC::Envelope& envelope, const char* command,
                      std::string* argument) {
    if (envelope.body.kind != RRC::BodyKind::Text) return false;
    const std::string& text = envelope.body.text;
    const size_t length = std::strlen(command);
    if (text.size() < length || text.compare(0, length, command) != 0) return false;
    if (text.size() > length && text[length] != ' ') return false;  // /whoever
    if (argument) {
        *argument = text.size() > length ? text.substr(length + 1) : std::string();
    }
    return true;
}

void send_notice(Slot& slot, const std::string& text,
                 const std::optional<std::string>& room) {
    RRC::Envelope notice = base_envelope(RRC::T_NOTICE);
    notice.room = room;
    notice.body = RRC::Body::text_value(text);
    send_envelope(slot, notice);
}

void reply_room_list(Slot& slot) {
    const RRC::AllRoomNames rooms = hub_state.all_rooms();
    send_notice(slot,
                RRC::format_room_list(rooms.values.data(), rooms.count,
                                      rrc_hub_max_body_bytes),
                std::nullopt);
}

void reply_member_list(Slot& slot, const std::string& room) {
    const RRC::MemberKeys members = hub_state.members(room);
    std::array<RRC::MemberEntry, RRC::HARD_MAX_SESSIONS> entries{};
    size_t count = 0;
    for (size_t i = 0; i < members.count && count < entries.size(); i++) {
        const auto identity = hub_state.identity(members.values[i]);
        if (!identity) continue;
        entries[count].identity = *identity;
        entries[count].nickname = hub_state.nickname(members.values[i]);
        ++count;
    }
    send_notice(slot,
                RRC::format_member_list(room, entries.data(), count,
                                        rrc_hub_max_body_bytes),
                room);
}

// Returns true when the envelope was a service command and has been answered.
bool handle_service_command(Slot& slot, const RRC::Envelope& envelope) {
    if (!hub_state.welcomed(slot.key)) return false;
    std::string argument;
    if (body_starts_with(envelope, "/list", &argument)) {
        reply_room_list(slot);
        ++accepted_count;
        return true;
    }
    if (body_starts_with(envelope, "/who", &argument) ||
        body_starts_with(envelope, "/names", &argument)) {
        // `/who` takes an optional room; NomadNet sends the name explicitly and
        // Eridanus does the same. Fall back to the envelope's room.
        std::string room = RRC::normalize_room(argument);
        if (room.empty() && envelope.room) room = RRC::normalize_room(*envelope.room);
        if (room.empty()) return false;
        reply_member_list(slot, room);
        ++accepted_count;
        return true;
    }
    return false;
}

void handle_room_traffic(Slot& slot, RRC::Envelope envelope, uint64_t now_ms) {
    // Service commands are answered privately and never relayed as room text.
    if (handle_service_command(slot, envelope)) return;

    // A text envelope with no room is a global hub command, not room traffic.
    // Stock NomadNet sends `/list` this way right after WELCOME. The MVP hub
    // implements no commands, and RRC requires unknown things to be ignored
    // rather than refused, so drop it silently: answering with an ERROR made
    // every stock client show a failure on a healthy connection.
    if (!envelope.room) return;
    const std::string room = RRC::normalize_room(*envelope.room);
    const RRC::StateError allowed = hub_state.can_send(slot.key, room, now_ms);
    if (allowed != RRC::StateError::None) {
        reject(slot, RRC::state_error_string(allowed));
        return;
    }
    const auto identity = hub_state.identity(slot.key);
    if (!identity) {
        reject(slot, "remote identity required");
        return;
    }
    const RRC::Error stamped = RRC::stamp_forwarded(
        envelope, *identity, hub_state.nickname(slot.key), validation_limits(slot));
    if (stamped != RRC::Error::None) {
        reject(slot, RRC::error_string(stamped));
        return;
    }
    ++accepted_count;
    forwarded_count += fanout(hub_state.members(room), envelope);
}

void handle_packet(const RNS::Bytes& plaintext, const RNS::Packet& packet) {
    Slot* slot = packet.link() ? find_slot(packet.link().link_id()) : nullptr;
    if (!slot) return;
    ++rx_count;

    // Link encryption alone does not prove who is at the other end. Until the
    // remote-identification callback succeeds, do not parse or answer any
    // application bytes (including a syntactically valid HELLO).
    if (!hub_state.identity(slot->key)) {
        ++rejected_count;
        return;
    }

    RRC::Envelope envelope;
    const RRC::Result decoded = RRC::decode(
        plaintext.data(), plaintext.size(), envelope, validation_limits(*slot));
    if (!decoded) {
        ++malformed_count;
        reject(*slot, RRC::error_string(decoded.error));
        return;
    }

    const uint64_t now_ms = monotonic_ms();
    if (envelope.type != RRC::T_PONG) {
        const RRC::StateError rate = hub_state.consume_rate(slot->key, now_ms);
        if (rate == RRC::StateError::RateLimited) {
            ++rate_limited_count;
            reject(*slot, RRC::state_error_string(rate));
            return;
        }
        if (rate != RRC::StateError::None) {
            // Expiry removes HubState before asynchronous Link teardown has
            // necessarily completed. A late packet in that window is stale,
            // not rate-limited, and must not receive another application reply.
            ++rejected_count;
            return;
        }
    }
    switch (envelope.type) {
        case RRC::T_HELLO:
            handle_hello(*slot, envelope, now_ms);
            break;
        case RRC::T_JOIN:
            handle_join(*slot, envelope);
            break;
        case RRC::T_PART:
            handle_part(*slot, envelope);
            break;
        case RRC::T_MSG:
        case RRC::T_NOTICE:
        case RRC::T_ACTION:
            handle_room_traffic(*slot, envelope, now_ms);
            break;
        case RRC::T_PING: {
            if (!hub_state.welcomed(slot->key)) {
                reject(*slot, "HELLO required");
                break;
            }
            RRC::Envelope pong = base_envelope(RRC::T_PONG);
            pong.body = envelope.body;
            send_envelope(*slot, pong);
            ++accepted_count;
            break;
        }
        case RRC::T_PONG:
            if (!hub_state.welcomed(slot->key)) {
                reject(*slot, "HELLO required");
            } else if (hub_state.pong(slot->key) == RRC::StateError::None) {
                ++accepted_count;
            }
            break;
        default:
            // RRC v1 requires forward compatibility: unknown message types
            // are valid envelopes but have no semantics for this hub.
            break;
    }
}

void handle_identified(const RNS::Link& link, const RNS::Identity& identity) {
    Slot* slot = find_slot(link.link_id());
    if (!slot) return;
    RRC::IdentityHash hash{};
    if (!copy_identity_hash(identity, hash) ||
        hub_state.identify(slot->key, hash, monotonic_ms()) != RRC::StateError::None) {
        ++rejected_count;
        RNS::Link closing = slot->link;
        closing.teardown();
    }
}

void handle_closed(RNS::Link& link) {
    Slot* slot = find_slot(link.link_id());
    if (!slot) return;
    const RRC::SessionKey key = slot->key;
    const auto identity = hub_state.identity(key);
    const auto nickname = hub_state.nickname(key);
    const RRC::RoomNames rooms = hub_state.joined_rooms(key);
    hub_state.close(slot->key);
    *slot = Slot{};

    if (!identity) return;
    for (size_t i = 0; i < rooms.count; ++i) {
        RRC::Envelope parted = base_envelope(RRC::T_PARTED);
        parted.room = rooms.values[i];
        parted.body = RRC::Body::member_list({*identity});
        parted.nickname = nickname;
        fanout(hub_state.members(rooms.values[i]), parted);
    }
}

void handle_established(RNS::Link& link) {
    Slot* slot = free_slot();
    if (!slot) {
        ++rejected_count;
        link.teardown();
        return;
    }
    RRC::SessionKey key = next_session_key++;
    if (key == 0) key = next_session_key++;
    if (hub_state.open(key, monotonic_ms()) != RRC::StateError::None) {
        ++rejected_count;
        link.teardown();
        return;
    }
    slot->used = true;
    slot->link = link;
    slot->link_id = link.link_id();
    slot->key = key;
    slot->last_ping_ms = monotonic_ms();
    // ACCEPT_NONE is the Link default. Do not pass it to
    // set_resource_strategy(): the currently pinned microReticulum release
    // validates strategies as a non-zero bitmask and throws for the zero-valued
    // ACCEPT_NONE constant, which would abort callback installation here.
    link.set_packet_callback(handle_packet);
    link.set_remote_identified_callback(handle_identified);
    link.set_link_closed_callback(handle_closed);
}

RNS::Bytes announce_data() {
    std::array<uint8_t, 128> buffer{};
    CborEncoder root;
    CborEncoder map;
    cbor_encoder_init(&root, buffer.data(), buffer.size(), 0);
    CborError error = cbor_encoder_create_map(&root, &map, 3);
    if (error == CborNoError) error = cbor_encode_text_stringz(&map, "proto");
    if (error == CborNoError) error = cbor_encode_text_stringz(&map, "rrc");
    if (error == CborNoError) error = cbor_encode_text_stringz(&map, "v");
    if (error == CborNoError) error = cbor_encode_uint(&map, RRC::VERSION);
    if (error == CborNoError) error = cbor_encode_text_stringz(&map, "hub");
    if (error == CborNoError) error = cbor_encode_text_stringz(&map, rrc_hub_name);
    if (error == CborNoError) error = cbor_encoder_close_container_checked(&root, &map);
    if (error != CborNoError) return {};
    const size_t size = cbor_encoder_get_buffer_size(&root, buffer.data());
    return RNS::Bytes(buffer.data(), size);
}

void announce() {
    if (!destination) return;
    const RNS::Bytes data = announce_data();
    if (data) destination.announce(data);
}

} // namespace

bool rrc_hub_enabled = true;
char rrc_hub_name[64] = "IMPR-RAD RRC";
uint32_t rrc_hub_announce_interval_seconds = 6U * 60U * 60U;
uint8_t rrc_hub_max_sessions = 8;
uint8_t rrc_hub_max_rooms_per_session = 4;
uint16_t rrc_hub_max_body_bytes = 280;
uint16_t rrc_hub_rate_per_minute = 60;
uint32_t rrc_hub_ping_interval_seconds = 60;
uint32_t rrc_hub_pong_timeout_seconds = 30;

void rrc_hub_begin(const RNS::Identity& identity) {
    if (!rrc_hub_enabled || !identity ||
        !copy_identity_hash(identity, hub_identity_hash)) return;
    hub_state = RRC::HubState(make_state_limits());
    slots = {};
    destination = RNS::Destination(identity, RNS::Type::Destination::IN,
                                   RNS::Type::Destination::SINGLE,
                                   "rrc", "hub");
    destination.set_link_established_callback(handle_established);
    announce();
    printf("[rrc] hub destination <%s>\n", destination.hash().toHex().c_str());
}

void rrc_hub_loop() {
    if (!destination) return;
    const uint64_t now_ms = monotonic_ms();

    RRC::ExpireCounts expired;
    hub_state.expire(now_ms, &expired);
    identify_timeout_count += expired.unidentified;
    hello_timeout_count += expired.hello;
    pong_timeout_count += expired.pong;
    for (auto& slot : slots) {
        if (!slot.used) continue;
        if (!hub_state.has_session(slot.key)) {
            RNS::Link closing = slot.link;
            closing.teardown();
            continue;
        }
        if (hub_state.welcomed(slot.key) &&
            !hub_state.awaiting_pong(slot.key) &&
            now_ms - slot.last_ping_ms >=
                static_cast<uint64_t>(rrc_hub_ping_interval_seconds) * 1000ULL) {
            RRC::Envelope ping = base_envelope(RRC::T_PING);
            std::array<uint8_t, 8> nonce{};
            esp_fill_random(nonce.data(), nonce.size());
            ping.body = RRC::Body::bytes_value(nonce.data(), nonce.size());
            if (send_envelope(slot, ping)) {
                hub_state.mark_ping_sent(slot.key, now_ms);
                slot.last_ping_ms = now_ms;
            }
        }
    }

    if (!announce_armed) {
        announce_armed = true;
        last_announce_ms = now_ms;
    } else {
        const uint64_t due = first_delayed_announce
            ? static_cast<uint64_t>(rrc_hub_announce_interval_seconds) * 1000ULL
            : ANNOUNCE_FIRST_MS;
        if (now_ms - last_announce_ms >= due) {
            announce();
            last_announce_ms = now_ms;
            first_delayed_announce = true;
        }
    }
}

RNS::Bytes rrc_hub_destination_hash() {
    return destination ? destination.hash() : RNS::Bytes{};
}

bool rrc_hub_running() { return static_cast<bool>(destination); }
size_t rrc_hub_session_count() { return hub_state.session_count(); }
size_t rrc_hub_identified_count() { return hub_state.identified_count(); }
size_t rrc_hub_room_count() { return hub_state.room_count(); }
size_t rrc_hub_membership_count() { return hub_state.membership_count(); }
uint32_t rrc_hub_rx_count() { return rx_count; }
uint32_t rrc_hub_tx_count() { return tx_count; }
uint32_t rrc_hub_accepted_count() { return accepted_count; }
uint32_t rrc_hub_forwarded_count() { return forwarded_count; }
uint32_t rrc_hub_rejected_count() { return rejected_count; }
uint32_t rrc_hub_rate_limited_count() { return rate_limited_count; }
uint32_t rrc_hub_malformed_count() { return malformed_count; }
uint32_t rrc_hub_identify_timeout_count() { return identify_timeout_count; }
uint32_t rrc_hub_hello_timeout_count() { return hello_timeout_count; }
uint32_t rrc_hub_pong_timeout_count() { return pong_timeout_count; }

#endif // RRC_HUB
