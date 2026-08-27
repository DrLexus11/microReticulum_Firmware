#pragma once

#include "RRCProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace RRC {

using SessionKey = uint64_t;

constexpr size_t HARD_MAX_SESSIONS = 16;
constexpr size_t HARD_MAX_ROOMS = 32;
constexpr size_t HARD_MAX_ROOMS_PER_SESSION = 8;

struct StateLimits {
    size_t max_sessions = 8;
    size_t max_rooms = 16;
    size_t max_rooms_per_session = 4;
    size_t max_room_name_bytes = MAX_ROOM_BYTES;
    size_t max_nick_bytes = MAX_NICK_BYTES;
    uint16_t messages_per_minute = 60;
    uint64_t identify_timeout_ms = 30000;
    uint64_t hello_timeout_ms = 30000;
    uint64_t pong_timeout_ms = 30000;
};

enum class StateError : uint8_t {
    None,
    LimitReached,
    AlreadyOpen,
    NotFound,
    NotIdentified,
    AlreadyIdentified,
    HelloRequired,
    AlreadyWelcomed,
    InvalidName,
    AlreadyJoined,
    NotJoined,
    RateLimited,
};

const char* state_error_string(StateError error);

struct MemberKeys {
    std::array<SessionKey, HARD_MAX_SESSIONS> values{};
    size_t count = 0;
};

struct RoomNames {
    std::array<std::string, HARD_MAX_ROOMS_PER_SESSION> values{};
    size_t count = 0;
};

struct ExpireCounts {
    size_t unidentified = 0;
    size_t hello = 0;
    size_t pong = 0;

    size_t total() const { return unidentified + hello + pong; }
};

class HubState {
public:
    explicit HubState(const StateLimits& limits = StateLimits{});

    StateError open(SessionKey key, uint64_t now_ms);
    StateError identify(SessionKey key, const IdentityHash& identity,
                        uint64_t now_ms);
    StateError hello(SessionKey key, const std::optional<std::string>& nickname,
                     uint64_t now_ms);
    StateError set_nickname(SessionKey key, const std::string& nickname);
    StateError join(SessionKey key, const std::string& room);
    StateError part(SessionKey key, const std::string& room);
    StateError consume_rate(SessionKey key, uint64_t now_ms);
    StateError can_send(SessionKey key, const std::string& room,
                        uint64_t now_ms);
    StateError pong(SessionKey key);
    StateError mark_ping_sent(SessionKey key, uint64_t now_ms);
    bool close(SessionKey key);
    size_t expire(uint64_t now_ms, ExpireCounts* counts = nullptr);

    size_t session_count() const;
    size_t identified_count() const;
    size_t room_count() const;
    size_t membership_count() const;
    bool has_session(SessionKey key) const;
    bool welcomed(SessionKey key) const;
    bool awaiting_pong(SessionKey key) const;
    bool joined(SessionKey key, const std::string& room) const;
    std::optional<IdentityHash> identity(SessionKey key) const;
    std::optional<std::string> nickname(SessionKey key) const;
    RoomNames joined_rooms(SessionKey key) const;
    MemberKeys members(const std::string& room) const;

private:
    struct Session {
        bool used = false;
        SessionKey key = 0;
        bool identified = false;
        bool welcomed = false;
        IdentityHash identity{};
        std::string nickname;
        uint64_t opened_ms = 0;
        uint64_t identified_ms = 0;
        uint64_t awaiting_pong_ms = 0;
        double rate_tokens = 0;
        uint64_t rate_refill_ms = 0;
        std::array<int8_t, HARD_MAX_ROOMS_PER_SESSION> rooms{};
        size_t room_count = 0;
    };

    struct Room {
        bool used = false;
        std::string name;
        std::array<int8_t, HARD_MAX_SESSIONS> members{};
        size_t member_count = 0;
    };

    StateLimits limits_;
    std::array<Session, HARD_MAX_SESSIONS> sessions_{};
    std::array<Room, HARD_MAX_ROOMS> rooms_{};

    int find_session(SessionKey key) const;
    int free_session() const;
    int find_room(const std::string& normalized) const;
    int free_room() const;
    bool consume_token(Session& session, uint64_t now_ms);
    void remove_session_from_room(size_t session_index, size_t room_index);
};

} // namespace RRC
