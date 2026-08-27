#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace RRC {

constexpr uint8_t VERSION = 1;
constexpr size_t MESSAGE_ID_BYTES = 8;
constexpr size_t IDENTITY_HASH_BYTES = 16;
constexpr size_t MAX_ENVELOPE_BYTES = 431;
constexpr size_t MAX_TEXT_BYTES = 280;
constexpr size_t MAX_ROOM_BYTES = 64;
constexpr size_t MAX_NICK_BYTES = 32;
constexpr size_t MAX_CLIENT_NAME_BYTES = 64;
constexpr size_t MAX_VERSION_BYTES = 32;
constexpr size_t MAX_MEMBERS = 8;

enum Key : uint8_t {
    K_V = 0,
    K_T = 1,
    K_ID = 2,
    K_TS = 3,
    K_SRC = 4,
    K_ROOM = 5,
    K_BODY = 6,
    K_NICK = 7,
};

enum MessageType : uint8_t {
    T_HELLO = 1,
    T_WELCOME = 2,
    T_JOIN = 10,
    T_JOINED = 11,
    T_PART = 12,
    T_PARTED = 13,
    T_MSG = 20,
    T_NOTICE = 21,
    T_ACTION = 22,
    T_PING = 30,
    T_PONG = 31,
    T_ERROR = 40,
    T_RESOURCE_ENVELOPE = 50,
};

enum HelloBodyKey : uint8_t {
    B_HELLO_NAME = 0,
    B_HELLO_VER = 1,
    B_HELLO_CAPS = 2,
};

enum WelcomeBodyKey : uint8_t {
    B_WELCOME_HUB = 0,
    B_WELCOME_VER = 1,
    B_WELCOME_CAPS = 2,
    B_WELCOME_LIMITS = 3,
};

enum LimitKey : uint8_t {
    L_MAX_NICK_BYTES = 0,
    L_MAX_ROOM_NAME_BYTES = 1,
    L_MAX_MSG_BODY_BYTES = 2,
    L_MAX_ROOMS_PER_SESSION = 3,
    L_RATE_LIMIT_MSGS_PER_MINUTE = 4,
};

enum CapabilityKey : uint8_t {
    CAP_RESOURCE_ENVELOPE = 0,
    CAP_ACTION = 1,
};

using MessageId = std::array<uint8_t, MESSAGE_ID_BYTES>;
using IdentityHash = std::array<uint8_t, IDENTITY_HASH_BYTES>;

enum class BodyKind : uint8_t {
    None,
    Text,
    Unsigned,
    Bytes,
    Members,
    Hello,
    Welcome,
};

struct Capabilities {
    bool resource_envelope = false;
    bool action = false;
};

struct HubLimits {
    uint16_t max_nick_bytes = MAX_NICK_BYTES;
    uint16_t max_room_name_bytes = MAX_ROOM_BYTES;
    uint16_t max_msg_body_bytes = MAX_TEXT_BYTES;
    uint8_t max_rooms_per_session = 4;
    uint16_t rate_limit_msgs_per_minute = 60;
};

struct HelloBody {
    std::optional<std::string> client_name;
    std::optional<std::string> client_version;
    Capabilities capabilities;
};

struct WelcomeBody {
    std::optional<std::string> hub_name;
    std::optional<std::string> hub_version;
    Capabilities capabilities;
    std::optional<HubLimits> limits;
};

struct Body {
    BodyKind kind = BodyKind::None;
    std::string text;
    uint64_t unsigned_value = 0;
    std::vector<uint8_t> bytes;
    std::vector<IdentityHash> members;
    HelloBody hello;
    WelcomeBody welcome;

    static Body text_value(const std::string& value);
    static Body unsigned_value_of(uint64_t value);
    static Body bytes_value(const uint8_t* value, size_t length);
    static Body member_list(const std::vector<IdentityHash>& value);
    static Body hello_value(const HelloBody& value);
    static Body welcome_value(const WelcomeBody& value);
};

struct Envelope {
    uint8_t version = VERSION;
    uint8_t type = 0;
    MessageId message_id{};
    uint64_t timestamp_ms = 0;
    IdentityHash source{};
    std::optional<std::string> room;
    Body body;
    std::optional<std::string> nickname;
};

enum class Error : uint8_t {
    None,
    Empty,
    TooLarge,
    Malformed,
    UnsupportedVersion,
    MissingField,
    DuplicateField,
    WrongType,
    WrongLength,
    InvalidUtf8,
    ConstraintViolation,
    OutputTooSmall,
};

const char* error_string(Error error);

struct Result {
    Error error = Error::None;
    size_t size = 0;

    explicit operator bool() const { return error == Error::None; }
};

struct ValidationLimits {
    size_t max_envelope_bytes = MAX_ENVELOPE_BYTES;
    size_t max_room_bytes = MAX_ROOM_BYTES;
    size_t max_nick_bytes = MAX_NICK_BYTES;
    size_t max_body_bytes = MAX_TEXT_BYTES;
    size_t max_members = MAX_MEMBERS;
};

bool is_utf8(const std::string& value);
std::string normalize_room(const std::string& room);

// Whether a client-supplied room token names the given canonical room.
//
// Clients are inconsistent about the leading '#': NomadNet sends "/who #room"
// while Eridanus sends "/who room" for the same room. Matching literally made
// the hub answer "(none)" for a populated room, which is indistinguishable from
// an empty one. Comparison is therefore case-insensitive and ignores a single
// leading '#' on either side. It deliberately does not strip anything else --
// '#' is the only prefix clients disagree about.
bool room_token_matches(const std::string& canonical, const std::string& token);
Error validate(const Envelope& envelope,
               const ValidationLimits& limits = ValidationLimits{});
Error stamp_forwarded(Envelope& envelope, const IdentityHash& authenticated_source,
                      const std::optional<std::string>& accepted_nickname,
                      const ValidationLimits& limits = ValidationLimits{});

// --- Hub service reply formats --------------------------------------------
//
// These strings are a client parser contract, not a presentation choice.
// NomadNet matches them with `_parse_room_list_notice` and `_parse_who_notice`;
// a wrong prefix or separator is not an error anywhere, it simply leaves the
// client's room list empty and its members unnamed. Kept here, away from
// Arduino and Reticulum, so the exact bytes can be asserted in native tests.

struct MemberEntry {
    IdentityHash identity{};
    std::optional<std::string> nickname;
};

// "Registered public rooms\n<room>\n<room>" or "No public rooms registered".
std::string format_room_list(const std::string* rooms, size_t count,
                             size_t max_bytes);

// "members in <room>: nick (hex12), <full-32-hex>, ..." or "... : (none)".
// A nicked member is identified by a 6-byte prefix, an un-nicked one by the
// whole 16-byte hash; that asymmetry is the client's format, not a shortcut.
std::string format_member_list(const std::string& room, const MemberEntry* members,
                               size_t count, size_t max_bytes);

Result encode(const Envelope& envelope, uint8_t* output, size_t capacity,
              const ValidationLimits& limits = ValidationLimits{});
Result decode(const uint8_t* data, size_t size, Envelope& envelope,
              const ValidationLimits& limits = ValidationLimits{});

} // namespace RRC
