#if defined(RRC_PROTOCOL_CORE)

#include "RRCState.h"

#include <algorithm>
#include <cctype>

namespace RRC {

namespace {

std::string trim(const std::string& value) {
    size_t begin = 0;
    while (begin < value.size() &&
           std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    size_t end = value.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(begin, end - begin);
}

} // namespace

const char* state_error_string(StateError error) {
    switch (error) {
        case StateError::None: return "none";
        case StateError::LimitReached: return "limit reached";
        case StateError::AlreadyOpen: return "session already open";
        case StateError::NotFound: return "session not found";
        case StateError::NotIdentified: return "remote identity required";
        case StateError::AlreadyIdentified: return "session already identified";
        case StateError::HelloRequired: return "HELLO required";
        case StateError::AlreadyWelcomed: return "session already welcomed";
        case StateError::InvalidName: return "invalid name";
        case StateError::AlreadyJoined: return "room already joined";
        case StateError::NotJoined: return "room not joined";
        case StateError::RateLimited: return "rate limited";
    }
    return "unknown";
}

HubState::HubState(const StateLimits& requested) : limits_(requested) {
    limits_.max_sessions = std::min(limits_.max_sessions, HARD_MAX_SESSIONS);
    limits_.max_rooms = std::min(limits_.max_rooms, HARD_MAX_ROOMS);
    limits_.max_rooms_per_session =
        std::min(limits_.max_rooms_per_session, HARD_MAX_ROOMS_PER_SESSION);
    for (auto& session : sessions_) session.rooms.fill(-1);
    for (auto& room : rooms_) room.members.fill(-1);
}

int HubState::find_session(SessionKey key) const {
    for (size_t i = 0; i < limits_.max_sessions; ++i) {
        if (sessions_[i].used && sessions_[i].key == key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HubState::free_session() const {
    for (size_t i = 0; i < limits_.max_sessions; ++i) {
        if (!sessions_[i].used) return static_cast<int>(i);
    }
    return -1;
}

int HubState::find_room(const std::string& normalized) const {
    for (size_t i = 0; i < limits_.max_rooms; ++i) {
        if (rooms_[i].used && rooms_[i].name == normalized) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int HubState::free_room() const {
    for (size_t i = 0; i < limits_.max_rooms; ++i) {
        if (!rooms_[i].used) return static_cast<int>(i);
    }
    return -1;
}

StateError HubState::open(SessionKey key, uint64_t now_ms) {
    if (find_session(key) >= 0) return StateError::AlreadyOpen;
    const int index = free_session();
    if (index < 0) return StateError::LimitReached;

    Session session;
    session.used = true;
    session.key = key;
    session.opened_ms = now_ms;
    session.rate_tokens = static_cast<double>(limits_.messages_per_minute);
    session.rate_refill_ms = now_ms;
    session.rooms.fill(-1);
    sessions_[static_cast<size_t>(index)] = session;
    return StateError::None;
}

StateError HubState::identify(SessionKey key, const IdentityHash& identity_value,
                              uint64_t now_ms) {
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    Session& session = sessions_[static_cast<size_t>(index)];
    if (session.identified) {
        // Re-identification with the same identity is idempotent, not an error.
        // Reticulum may deliver the identified callback more than once, and the
        // hub also reads the Link's proven identity directly when a packet
        // arrives before the callback has landed -- so a second, identical
        // identify is entirely normal. Treating it as a failure made the caller
        // tear down a session that had just identified correctly, which
        // presented to stock clients as "identified, sending HELLO" followed by
        // a disconnect on a hub that was otherwise healthy.
        //
        // A *different* identity on an established session is another matter
        // and is still refused: the caller must not silently re-attribute a
        // live session to someone else.
        return session.identity == identity_value ? StateError::None
                                                  : StateError::AlreadyIdentified;
    }
    session.identified = true;
    session.identity = identity_value;
    session.identified_ms = now_ms;
    return StateError::None;
}

StateError HubState::hello(SessionKey key,
                           const std::optional<std::string>& nickname_value,
                           uint64_t now_ms) {
    (void)now_ms;
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    Session& session = sessions_[static_cast<size_t>(index)];
    if (!session.identified) return StateError::NotIdentified;
    if (session.welcomed) return StateError::AlreadyWelcomed;
    if (nickname_value) {
        const StateError result = set_nickname(key, *nickname_value);
        if (result != StateError::None) return result;
    }
    session.welcomed = true;
    return StateError::None;
}

StateError HubState::set_nickname(SessionKey key, const std::string& value) {
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    const std::string normalized = trim(value);
    if (normalized.empty() || normalized.size() > limits_.max_nick_bytes ||
        !is_utf8(normalized)) {
        return StateError::InvalidName;
    }
    sessions_[static_cast<size_t>(index)].nickname = normalized;
    return StateError::None;
}

StateError HubState::join(SessionKey key, const std::string& value) {
    const int session_index_raw = find_session(key);
    if (session_index_raw < 0) return StateError::NotFound;
    const size_t session_index = static_cast<size_t>(session_index_raw);
    Session& session = sessions_[session_index];
    if (!session.identified) return StateError::NotIdentified;
    if (!session.welcomed) return StateError::HelloRequired;

    const std::string room_name = normalize_room(value);
    if (room_name.empty() || room_name.size() > limits_.max_room_name_bytes ||
        !is_utf8(room_name)) {
        return StateError::InvalidName;
    }

    int room_index_raw = find_room(room_name);
    if (room_index_raw >= 0) {
        for (size_t i = 0; i < session.room_count; ++i) {
            if (session.rooms[i] == room_index_raw) return StateError::AlreadyJoined;
        }
    }
    if (session.room_count >= limits_.max_rooms_per_session) {
        return StateError::LimitReached;
    }

    if (room_index_raw < 0) {
        room_index_raw = free_room();
        if (room_index_raw < 0) return StateError::LimitReached;
        Room room;
        room.used = true;
        room.name = room_name;
        room.members.fill(-1);
        rooms_[static_cast<size_t>(room_index_raw)] = room;
    }

    Room& room = rooms_[static_cast<size_t>(room_index_raw)];
    if (room.member_count >= limits_.max_sessions) return StateError::LimitReached;
    session.rooms[session.room_count++] = static_cast<int8_t>(room_index_raw);
    room.members[room.member_count++] = static_cast<int8_t>(session_index);
    return StateError::None;
}

void HubState::remove_session_from_room(size_t session_index, size_t room_index) {
    Session& session = sessions_[session_index];
    Room& room = rooms_[room_index];

    for (size_t i = 0; i < session.room_count; ++i) {
        if (session.rooms[i] == static_cast<int8_t>(room_index)) {
            for (size_t j = i + 1; j < session.room_count; ++j) {
                session.rooms[j - 1] = session.rooms[j];
            }
            session.rooms[--session.room_count] = -1;
            break;
        }
    }

    for (size_t i = 0; i < room.member_count; ++i) {
        if (room.members[i] == static_cast<int8_t>(session_index)) {
            for (size_t j = i + 1; j < room.member_count; ++j) {
                room.members[j - 1] = room.members[j];
            }
            room.members[--room.member_count] = -1;
            break;
        }
    }

    if (room.member_count == 0) {
        room = Room{};
        room.members.fill(-1);
    }
}

StateError HubState::part(SessionKey key, const std::string& value) {
    const int session_index_raw = find_session(key);
    if (session_index_raw < 0) return StateError::NotFound;
    Session& session = sessions_[static_cast<size_t>(session_index_raw)];
    if (!session.welcomed) return StateError::HelloRequired;
    const int room_index_raw = find_room(normalize_room(value));
    if (room_index_raw < 0) return StateError::NotJoined;
    bool is_member = false;
    for (size_t i = 0; i < session.room_count; ++i) {
        if (session.rooms[i] == room_index_raw) {
            is_member = true;
            break;
        }
    }
    if (!is_member) return StateError::NotJoined;
    remove_session_from_room(static_cast<size_t>(session_index_raw),
                             static_cast<size_t>(room_index_raw));
    return StateError::None;
}

bool HubState::consume_token(Session& session, uint64_t now_ms) {
    if (limits_.messages_per_minute == 0) return false;
    if (now_ms > session.rate_refill_ms) {
        const double elapsed_ms = static_cast<double>(now_ms - session.rate_refill_ms);
        const double added = elapsed_ms * limits_.messages_per_minute / 60000.0;
        session.rate_tokens = std::min(
            static_cast<double>(limits_.messages_per_minute),
            session.rate_tokens + added);
        session.rate_refill_ms = now_ms;
    }
    if (session.rate_tokens < 1.0) return false;
    session.rate_tokens -= 1.0;
    return true;
}

StateError HubState::can_send(SessionKey key, const std::string& value,
                              uint64_t now_ms) {
    (void)now_ms;
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    Session& session = sessions_[static_cast<size_t>(index)];
    if (!session.identified) return StateError::NotIdentified;
    if (!session.welcomed) return StateError::HelloRequired;
    if (!joined(key, value)) return StateError::NotJoined;
    return StateError::None;
}

StateError HubState::consume_rate(SessionKey key, uint64_t now_ms) {
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    return consume_token(sessions_[static_cast<size_t>(index)], now_ms)
        ? StateError::None : StateError::RateLimited;
}

StateError HubState::pong(SessionKey key) {
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    sessions_[static_cast<size_t>(index)].awaiting_pong_ms = 0;
    return StateError::None;
}

StateError HubState::mark_ping_sent(SessionKey key, uint64_t now_ms) {
    const int index = find_session(key);
    if (index < 0) return StateError::NotFound;
    Session& session = sessions_[static_cast<size_t>(index)];
    if (!session.welcomed) return StateError::HelloRequired;
    // Preserve the first unanswered PING's timestamp. Re-marking a pending
    // check must never extend the PONG deadline.
    if (session.awaiting_pong_ms == 0) {
        session.awaiting_pong_ms = now_ms == 0 ? 1 : now_ms;
    }
    return StateError::None;
}

bool HubState::close(SessionKey key) {
    const int index_raw = find_session(key);
    if (index_raw < 0) return false;
    const size_t index = static_cast<size_t>(index_raw);
    while (sessions_[index].room_count > 0) {
        const int8_t room_index = sessions_[index].rooms[0];
        if (room_index < 0) break;
        remove_session_from_room(index, static_cast<size_t>(room_index));
    }
    sessions_[index] = Session{};
    sessions_[index].rooms.fill(-1);
    return true;
}

size_t HubState::expire(uint64_t now_ms, ExpireCounts* counts) {
    std::array<SessionKey, HARD_MAX_SESSIONS> expired{};
    size_t count = 0;
    for (size_t i = 0; i < limits_.max_sessions; ++i) {
        const Session& session = sessions_[i];
        if (!session.used) continue;
        bool should_expire = false;
        if (!session.identified) {
            should_expire = now_ms >= session.opened_ms &&
                now_ms - session.opened_ms >= limits_.identify_timeout_ms;
            if (should_expire && counts) ++counts->unidentified;
        } else if (!session.welcomed) {
            should_expire = now_ms >= session.identified_ms &&
                now_ms - session.identified_ms >= limits_.hello_timeout_ms;
            if (should_expire && counts) ++counts->hello;
        } else if (session.awaiting_pong_ms != 0) {
            should_expire = now_ms >= session.awaiting_pong_ms &&
                now_ms - session.awaiting_pong_ms >= limits_.pong_timeout_ms;
            if (should_expire && counts) ++counts->pong;
        }
        if (should_expire) expired[count++] = session.key;
    }
    for (size_t i = 0; i < count; ++i) close(expired[i]);
    return count;
}

size_t HubState::session_count() const {
    size_t count = 0;
    for (const auto& session : sessions_) if (session.used) ++count;
    return count;
}

size_t HubState::identified_count() const {
    size_t count = 0;
    for (const auto& session : sessions_) {
        if (session.used && session.identified) ++count;
    }
    return count;
}

size_t HubState::room_count() const {
    size_t count = 0;
    for (const auto& room : rooms_) if (room.used) ++count;
    return count;
}

size_t HubState::membership_count() const {
    size_t count = 0;
    for (const auto& session : sessions_) {
        if (session.used) count += session.room_count;
    }
    return count;
}

bool HubState::has_session(SessionKey key) const {
    return find_session(key) >= 0;
}

bool HubState::welcomed(SessionKey key) const {
    const int index = find_session(key);
    return index >= 0 && sessions_[static_cast<size_t>(index)].welcomed;
}

bool HubState::awaiting_pong(SessionKey key) const {
    const int index = find_session(key);
    return index >= 0 &&
        sessions_[static_cast<size_t>(index)].awaiting_pong_ms != 0;
}

bool HubState::joined(SessionKey key, const std::string& value) const {
    const int session_index = find_session(key);
    const int room_index = find_room(normalize_room(value));
    if (session_index < 0 || room_index < 0) return false;
    const Session& session = sessions_[static_cast<size_t>(session_index)];
    for (size_t i = 0; i < session.room_count; ++i) {
        if (session.rooms[i] == room_index) return true;
    }
    return false;
}

std::optional<IdentityHash> HubState::identity(SessionKey key) const {
    const int index = find_session(key);
    if (index < 0 || !sessions_[static_cast<size_t>(index)].identified) {
        return std::nullopt;
    }
    return sessions_[static_cast<size_t>(index)].identity;
}

std::optional<std::string> HubState::nickname(SessionKey key) const {
    const int index = find_session(key);
    if (index < 0 || sessions_[static_cast<size_t>(index)].nickname.empty()) {
        return std::nullopt;
    }
    return sessions_[static_cast<size_t>(index)].nickname;
}

RoomNames HubState::joined_rooms(SessionKey key) const {
    RoomNames result;
    const int index = find_session(key);
    if (index < 0) return result;
    const Session& session = sessions_[static_cast<size_t>(index)];
    for (size_t i = 0; i < session.room_count; ++i) {
        const int8_t room_index = session.rooms[i];
        if (room_index >= 0 && rooms_[static_cast<size_t>(room_index)].used) {
            result.values[result.count++] =
                rooms_[static_cast<size_t>(room_index)].name;
        }
    }
    return result;
}

AllRoomNames HubState::all_rooms() const {
    AllRoomNames names;
    for (const auto& room : rooms_) {
        if (!room.used || names.count >= names.values.size()) continue;
        names.values[names.count++] = room.name;
    }
    return names;
}

MemberKeys HubState::members(const std::string& value) const {
    MemberKeys result;
    const int room_index = find_room(normalize_room(value));
    if (room_index < 0) return result;
    const Room& room = rooms_[static_cast<size_t>(room_index)];
    for (size_t i = 0; i < room.member_count; ++i) {
        const int8_t session_index = room.members[i];
        if (session_index >= 0 && sessions_[static_cast<size_t>(session_index)].used) {
            result.values[result.count++] =
                sessions_[static_cast<size_t>(session_index)].key;
        }
    }
    return result;
}

} // namespace RRC

#endif // RRC_PROTOCOL_CORE
