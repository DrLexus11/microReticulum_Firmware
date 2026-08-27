#if defined(RRC_PROTOCOL_CORE)

#include "RRCProtocol.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include <cbor.h>

namespace RRC {

namespace {

Error cbor_error(CborError error) {
    if (error == CborNoError) return Error::None;
    if (error == CborErrorOutOfMemory) return Error::OutputTooSmall;
    return Error::Malformed;
}

bool is_room_type(uint8_t type) {
    return type == T_JOIN || type == T_JOINED || type == T_PART ||
           type == T_PARTED || type == T_MSG || type == T_NOTICE ||
           type == T_ACTION;
}

bool is_text_type(uint8_t type) {
    return type == T_MSG || type == T_NOTICE || type == T_ACTION ||
           type == T_ERROR;
}

size_t body_map_size(const Body& body) {
    if (body.kind == BodyKind::Hello) {
        return (body.hello.client_name ? 1 : 0) +
               (body.hello.client_version ? 1 : 0) + 1;
    }
    if (body.kind == BodyKind::Welcome) {
        return (body.welcome.hub_name ? 1 : 0) +
               (body.welcome.hub_version ? 1 : 0) + 1 +
               (body.welcome.limits ? 1 : 0);
    }
    return 0;
}

CborError encode_capabilities(CborEncoder* parent,
                              const Capabilities& capabilities) {
    CborEncoder map;
    CborError error = cbor_encoder_create_map(parent, &map, 2);
    if (error != CborNoError) return error;
    if ((error = cbor_encode_uint(&map, CAP_RESOURCE_ENVELOPE)) != CborNoError ||
        (error = cbor_encode_boolean(&map, capabilities.resource_envelope)) != CborNoError ||
        (error = cbor_encode_uint(&map, CAP_ACTION)) != CborNoError ||
        (error = cbor_encode_boolean(&map, capabilities.action)) != CborNoError) {
        return error;
    }
    return cbor_encoder_close_container_checked(parent, &map);
}

CborError encode_limits(CborEncoder* parent, const HubLimits& limits) {
    CborEncoder map;
    CborError error = cbor_encoder_create_map(parent, &map, 5);
    if (error != CborNoError) return error;
    const std::array<std::pair<uint8_t, uint64_t>, 5> values{{
        {L_MAX_NICK_BYTES, limits.max_nick_bytes},
        {L_MAX_ROOM_NAME_BYTES, limits.max_room_name_bytes},
        {L_MAX_MSG_BODY_BYTES, limits.max_msg_body_bytes},
        {L_MAX_ROOMS_PER_SESSION, limits.max_rooms_per_session},
        {L_RATE_LIMIT_MSGS_PER_MINUTE, limits.rate_limit_msgs_per_minute},
    }};
    for (const auto& value : values) {
        if ((error = cbor_encode_uint(&map, value.first)) != CborNoError ||
            (error = cbor_encode_uint(&map, value.second)) != CborNoError) {
            return error;
        }
    }
    return cbor_encoder_close_container_checked(parent, &map);
}

CborError encode_body(CborEncoder* encoder, const Body& body) {
    CborError error = CborNoError;
    switch (body.kind) {
        case BodyKind::None:
            return CborNoError;
        case BodyKind::Text:
            return cbor_encode_text_string(encoder, body.text.data(), body.text.size());
        case BodyKind::Unsigned:
            return cbor_encode_uint(encoder, body.unsigned_value);
        case BodyKind::Bytes:
            return cbor_encode_byte_string(encoder, body.bytes.data(), body.bytes.size());
        case BodyKind::Members: {
            CborEncoder array;
            error = cbor_encoder_create_array(encoder, &array, body.members.size());
            if (error != CborNoError) return error;
            for (const auto& member : body.members) {
                error = cbor_encode_byte_string(&array, member.data(), member.size());
                if (error != CborNoError) return error;
            }
            return cbor_encoder_close_container_checked(encoder, &array);
        }
        case BodyKind::Hello: {
            CborEncoder map;
            error = cbor_encoder_create_map(encoder, &map, body_map_size(body));
            if (error != CborNoError) return error;
            if (body.hello.client_name) {
                if ((error = cbor_encode_uint(&map, B_HELLO_NAME)) != CborNoError ||
                    (error = cbor_encode_text_string(&map,
                        body.hello.client_name->data(),
                        body.hello.client_name->size())) != CborNoError) return error;
            }
            if (body.hello.client_version) {
                if ((error = cbor_encode_uint(&map, B_HELLO_VER)) != CborNoError ||
                    (error = cbor_encode_text_string(&map,
                        body.hello.client_version->data(),
                        body.hello.client_version->size())) != CborNoError) return error;
            }
            if ((error = cbor_encode_uint(&map, B_HELLO_CAPS)) != CborNoError ||
                (error = encode_capabilities(&map,
                    body.hello.capabilities)) != CborNoError) return error;
            return cbor_encoder_close_container_checked(encoder, &map);
        }
        case BodyKind::Welcome: {
            CborEncoder map;
            error = cbor_encoder_create_map(encoder, &map, body_map_size(body));
            if (error != CborNoError) return error;
            if (body.welcome.hub_name) {
                if ((error = cbor_encode_uint(&map, B_WELCOME_HUB)) != CborNoError ||
                    (error = cbor_encode_text_string(&map,
                        body.welcome.hub_name->data(),
                        body.welcome.hub_name->size())) != CborNoError) return error;
            }
            if (body.welcome.hub_version) {
                if ((error = cbor_encode_uint(&map, B_WELCOME_VER)) != CborNoError ||
                    (error = cbor_encode_text_string(&map,
                        body.welcome.hub_version->data(),
                        body.welcome.hub_version->size())) != CborNoError) return error;
            }
            if ((error = cbor_encode_uint(&map, B_WELCOME_CAPS)) != CborNoError ||
                (error = encode_capabilities(&map,
                    body.welcome.capabilities)) != CborNoError) return error;
            if (body.welcome.limits) {
                if ((error = cbor_encode_uint(&map, B_WELCOME_LIMITS)) != CborNoError ||
                    (error = encode_limits(&map, *body.welcome.limits)) != CborNoError) {
                    return error;
                }
            }
            return cbor_encoder_close_container_checked(encoder, &map);
        }
    }
    return CborErrorUnknownType;
}

Error read_uint(CborValue& value, uint64_t& output) {
    if (!cbor_value_is_unsigned_integer(&value)) return Error::WrongType;
    CborError error = cbor_value_get_uint64(&value, &output);
    if (error != CborNoError) return cbor_error(error);
    return cbor_error(cbor_value_advance_fixed(&value));
}

Error read_bool(CborValue& value, bool& output) {
    if (!cbor_value_is_boolean(&value)) return Error::WrongType;
    CborError error = cbor_value_get_boolean(&value, &output);
    if (error != CborNoError) return cbor_error(error);
    return cbor_error(cbor_value_advance_fixed(&value));
}

Error read_text(CborValue& value, size_t maximum, std::string& output) {
    if (!cbor_value_is_text_string(&value)) return Error::WrongType;
    size_t length = 0;
    CborError error = cbor_value_get_string_length(&value, &length);
    if (error != CborNoError) return cbor_error(error);
    if (length > maximum) return Error::ConstraintViolation;
    // TinyCBOR does not require space for its optional NUL terminator, but
    // providing it makes the ownership boundary explicit and avoids relying on
    // that less-obvious API detail.
    output.resize(length + 1);
    CborValue next;
    size_t copied = output.size();
    error = cbor_value_copy_text_string(&value, output.data(), &copied, &next);
    if (error != CborNoError) return cbor_error(error);
    // TinyCBOR variants disagree on whether the returned count includes the
    // trailing NUL. The destination was sized for either convention; only the
    // original text bytes become part of the std::string.
    if (copied != length && copied != length + 1) return Error::Malformed;
    output.resize(length);
    value = next;
    if (!is_utf8(output)) return Error::InvalidUtf8;
    return Error::None;
}

Error read_bytes(CborValue& value, size_t maximum, std::vector<uint8_t>& output) {
    if (!cbor_value_is_byte_string(&value)) return Error::WrongType;
    size_t length = 0;
    CborError error = cbor_value_get_string_length(&value, &length);
    if (error != CborNoError) return cbor_error(error);
    if (length > maximum) return Error::ConstraintViolation;
    output.resize(length);
    uint8_t empty = 0;
    uint8_t* destination = length == 0 ? &empty : output.data();
    CborValue next;
    size_t copied = length;
    error = cbor_value_copy_byte_string(&value, destination, &copied, &next);
    if (error != CborNoError) return cbor_error(error);
    value = next;
    return Error::None;
}

template <size_t N>
Error read_fixed_bytes(CborValue& value, std::array<uint8_t, N>& output) {
    if (!cbor_value_is_byte_string(&value)) return Error::WrongType;
    size_t length = 0;
    CborError error = cbor_value_get_string_length(&value, &length);
    if (error != CborNoError) return cbor_error(error);
    if (length != N) return Error::WrongLength;
    CborValue next;
    size_t copied = N;
    error = cbor_value_copy_byte_string(&value, output.data(), &copied, &next);
    if (error != CborNoError) return cbor_error(error);
    value = next;
    return Error::None;
}

Error skip(CborValue& value) {
    return cbor_error(cbor_value_advance(&value));
}

Error require_definite_lengths(CborValue& value, uint8_t depth = 0) {
    if (depth > 16 || !cbor_value_is_length_known(&value)) {
        return Error::Malformed;
    }
    if (!cbor_value_is_container(&value)) return skip(value);

    CborValue child;
    CborError cerror = cbor_value_enter_container(&value, &child);
    if (cerror != CborNoError) return cbor_error(cerror);
    while (!cbor_value_at_end(&child)) {
        Error error = require_definite_lengths(child, depth + 1);
        if (error != Error::None) return error;
    }
    return cbor_error(cbor_value_leave_container(&value, &child));
}

Error read_capabilities(CborValue& value, Capabilities& capabilities) {
    if (!cbor_value_is_map(&value)) return Error::WrongType;
    size_t count = 0;
    if (cbor_value_get_map_length(&value, &count) != CborNoError || count > 16) {
        return Error::ConstraintViolation;
    }
    CborValue map;
    CborError cerror = cbor_value_enter_container(&value, &map);
    if (cerror != CborNoError) return cbor_error(cerror);
    bool seen_resource = false;
    bool seen_action = false;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        Error error = read_uint(map, key);
        if (error != Error::None) return error;
        if (key == CAP_RESOURCE_ENVELOPE || key == CAP_ACTION) {
            bool enabled = false;
            error = read_bool(map, enabled);
            if (error != Error::None) return error;
            bool& seen = key == CAP_RESOURCE_ENVELOPE ? seen_resource : seen_action;
            if (seen) return Error::DuplicateField;
            seen = true;
            if (key == CAP_RESOURCE_ENVELOPE) capabilities.resource_envelope = enabled;
            else capabilities.action = enabled;
        } else {
            error = skip(map);
            if (error != Error::None) return error;
        }
    }
    cerror = cbor_value_leave_container(&value, &map);
    return cbor_error(cerror);
}

Error read_limits(CborValue& value, HubLimits& limits) {
    if (!cbor_value_is_map(&value)) return Error::WrongType;
    size_t count = 0;
    if (cbor_value_get_map_length(&value, &count) != CborNoError || count > 16) {
        return Error::ConstraintViolation;
    }
    CborValue map;
    CborError cerror = cbor_value_enter_container(&value, &map);
    if (cerror != CborNoError) return cbor_error(cerror);
    uint32_t seen = 0;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        Error error = read_uint(map, key);
        if (error != Error::None) return error;
        if (key <= L_RATE_LIMIT_MSGS_PER_MINUTE) {
            const uint32_t bit = 1u << key;
            if (seen & bit) return Error::DuplicateField;
            seen |= bit;
            uint64_t number = 0;
            error = read_uint(map, number);
            if (error != Error::None) return error;
            if (number > UINT16_MAX) return Error::ConstraintViolation;
            switch (key) {
                case L_MAX_NICK_BYTES: limits.max_nick_bytes = number; break;
                case L_MAX_ROOM_NAME_BYTES: limits.max_room_name_bytes = number; break;
                case L_MAX_MSG_BODY_BYTES: limits.max_msg_body_bytes = number; break;
                case L_MAX_ROOMS_PER_SESSION:
                    if (number > UINT8_MAX) return Error::ConstraintViolation;
                    limits.max_rooms_per_session = number;
                    break;
                case L_RATE_LIMIT_MSGS_PER_MINUTE:
                    limits.rate_limit_msgs_per_minute = number;
                    break;
            }
        } else {
            error = skip(map);
            if (error != Error::None) return error;
        }
    }
    cerror = cbor_value_leave_container(&value, &map);
    return cbor_error(cerror);
}

Error read_hello(CborValue& value, HelloBody& hello) {
    if (!cbor_value_is_map(&value)) return Error::WrongType;
    size_t count = 0;
    if (cbor_value_get_map_length(&value, &count) != CborNoError || count > 16) {
        return Error::ConstraintViolation;
    }
    CborValue map;
    CborError cerror = cbor_value_enter_container(&value, &map);
    if (cerror != CborNoError) return cbor_error(cerror);
    uint8_t seen = 0;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        Error error = read_uint(map, key);
        if (error != Error::None) return error;
        if (key <= B_HELLO_CAPS) {
            const uint8_t bit = static_cast<uint8_t>(1u << key);
            if (seen & bit) return Error::DuplicateField;
            seen |= bit;
            if (key == B_HELLO_NAME || key == B_HELLO_VER) {
                std::string text;
                error = read_text(map,
                    key == B_HELLO_NAME ? MAX_CLIENT_NAME_BYTES : MAX_VERSION_BYTES,
                    text);
                if (error == Error::None) {
                    if (key == B_HELLO_NAME) hello.client_name = text;
                    else hello.client_version = text;
                }
            } else {
                error = read_capabilities(map, hello.capabilities);
            }
            if (error != Error::None) return error;
        } else {
            error = skip(map);
            if (error != Error::None) return error;
        }
    }
    cerror = cbor_value_leave_container(&value, &map);
    return cbor_error(cerror);
}

Error read_welcome(CborValue& value, WelcomeBody& welcome) {
    if (!cbor_value_is_map(&value)) return Error::WrongType;
    size_t count = 0;
    if (cbor_value_get_map_length(&value, &count) != CborNoError || count > 16) {
        return Error::ConstraintViolation;
    }
    CborValue map;
    CborError cerror = cbor_value_enter_container(&value, &map);
    if (cerror != CborNoError) return cbor_error(cerror);
    uint8_t seen = 0;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        Error error = read_uint(map, key);
        if (error != Error::None) return error;
        if (key <= B_WELCOME_LIMITS) {
            const uint8_t bit = static_cast<uint8_t>(1u << key);
            if (seen & bit) return Error::DuplicateField;
            seen |= bit;
            if (key == B_WELCOME_HUB || key == B_WELCOME_VER) {
                std::string text;
                error = read_text(map,
                    key == B_WELCOME_HUB ? MAX_CLIENT_NAME_BYTES : MAX_VERSION_BYTES,
                    text);
                if (error == Error::None) {
                    if (key == B_WELCOME_HUB) welcome.hub_name = text;
                    else welcome.hub_version = text;
                }
            } else if (key == B_WELCOME_CAPS) {
                error = read_capabilities(map, welcome.capabilities);
            } else {
                HubLimits limits;
                error = read_limits(map, limits);
                if (error == Error::None) welcome.limits = limits;
            }
            if (error != Error::None) return error;
        } else {
            error = skip(map);
            if (error != Error::None) return error;
        }
    }
    cerror = cbor_value_leave_container(&value, &map);
    return cbor_error(cerror);
}

Error read_members(CborValue& value, size_t maximum,
                   std::vector<IdentityHash>& members) {
    if (!cbor_value_is_array(&value)) return Error::WrongType;
    size_t count = 0;
    if (cbor_value_get_array_length(&value, &count) != CborNoError ||
        count > maximum) return Error::ConstraintViolation;
    CborValue array;
    CborError cerror = cbor_value_enter_container(&value, &array);
    if (cerror != CborNoError) return cbor_error(cerror);
    members.reserve(count);
    while (!cbor_value_at_end(&array)) {
        IdentityHash member{};
        Error error = read_fixed_bytes(array, member);
        if (error != Error::None) return error;
        members.push_back(member);
    }
    cerror = cbor_value_leave_container(&value, &array);
    return cbor_error(cerror);
}

Error read_body(CborValue& value, uint8_t type, Body& body,
                const ValidationLimits& limits) {
    if (type == T_HELLO) {
        body.kind = BodyKind::Hello;
        return read_hello(value, body.hello);
    }
    if (type == T_WELCOME) {
        body.kind = BodyKind::Welcome;
        return read_welcome(value, body.welcome);
    }
    if (type == T_JOINED || type == T_PARTED) {
        body.kind = BodyKind::Members;
        return read_members(value, limits.max_members, body.members);
    }
    if (is_text_type(type)) {
        body.kind = BodyKind::Text;
        return read_text(value, limits.max_body_bytes, body.text);
    }
    if (type == T_PING || type == T_PONG) {
        if (cbor_value_is_unsigned_integer(&value)) {
            body.kind = BodyKind::Unsigned;
            return read_uint(value, body.unsigned_value);
        }
        if (cbor_value_is_text_string(&value)) {
            body.kind = BodyKind::Text;
            return read_text(value, limits.max_body_bytes, body.text);
        }
        if (cbor_value_is_byte_string(&value)) {
            body.kind = BodyKind::Bytes;
            return read_bytes(value, limits.max_body_bytes, body.bytes);
        }
        return Error::WrongType;
    }
    return skip(value);
}

Error find_type(const CborValue& root, uint8_t& type) {
    CborValue map;
    CborValue parent = root;
    CborError cerror = cbor_value_enter_container(&parent, &map);
    if (cerror != CborNoError) return cbor_error(cerror);
    bool found = false;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        Error error = read_uint(map, key);
        if (error != Error::None) return error;
        if (key == K_T) {
            if (found) return Error::DuplicateField;
            uint64_t value = 0;
            error = read_uint(map, value);
            if (error != Error::None) return error;
            if (value > UINT8_MAX) return Error::ConstraintViolation;
            type = static_cast<uint8_t>(value);
            found = true;
        } else {
            error = skip(map);
            if (error != Error::None) return error;
        }
    }
    return found ? Error::None : Error::MissingField;
}

} // namespace

Body Body::text_value(const std::string& value) {
    Body body;
    body.kind = BodyKind::Text;
    body.text = value;
    return body;
}

Body Body::unsigned_value_of(uint64_t value) {
    Body body;
    body.kind = BodyKind::Unsigned;
    body.unsigned_value = value;
    return body;
}

Body Body::bytes_value(const uint8_t* value, size_t length) {
    Body body;
    body.kind = BodyKind::Bytes;
    if (value != nullptr && length != 0) {
        body.bytes.assign(value, value + length);
    }
    return body;
}

Body Body::member_list(const std::vector<IdentityHash>& value) {
    Body body;
    body.kind = BodyKind::Members;
    body.members = value;
    return body;
}

Body Body::hello_value(const HelloBody& value) {
    Body body;
    body.kind = BodyKind::Hello;
    body.hello = value;
    return body;
}

Body Body::welcome_value(const WelcomeBody& value) {
    Body body;
    body.kind = BodyKind::Welcome;
    body.welcome = value;
    return body;
}

const char* error_string(Error error) {
    switch (error) {
        case Error::None: return "none";
        case Error::Empty: return "empty input";
        case Error::TooLarge: return "input too large";
        case Error::Malformed: return "malformed CBOR";
        case Error::UnsupportedVersion: return "unsupported RRC version";
        case Error::MissingField: return "missing required field";
        case Error::DuplicateField: return "duplicate field";
        case Error::WrongType: return "wrong field type";
        case Error::WrongLength: return "wrong field length";
        case Error::InvalidUtf8: return "invalid UTF-8";
        case Error::ConstraintViolation: return "constraint violation";
        case Error::OutputTooSmall: return "output buffer too small";
    }
    return "unknown";
}

bool is_utf8(const std::string& value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(value.data());
    size_t i = 0;
    while (i < value.size()) {
        const uint8_t first = bytes[i++];
        if (first <= 0x7f) continue;
        uint32_t codepoint = 0;
        size_t continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            codepoint = first & 0x1f;
            continuation = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            codepoint = first & 0x0f;
            continuation = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            codepoint = first & 0x07;
            continuation = 3;
        } else {
            return false;
        }
        if (i + continuation > value.size()) return false;
        for (size_t j = 0; j < continuation; ++j) {
            const uint8_t next = bytes[i++];
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation == 2 && codepoint < 0x800) ||
            (continuation == 3 && codepoint < 0x10000) ||
            (codepoint >= 0xd800 && codepoint <= 0xdfff) ||
            codepoint > 0x10ffff) return false;
    }
    return true;
}

std::string normalize_room(const std::string& room) {
    size_t begin = 0;
    while (begin < room.size() &&
           std::isspace(static_cast<unsigned char>(room[begin]))) ++begin;
    size_t end = room.size();
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(room[end - 1]))) --end;
    std::string normalized = room.substr(begin, end - begin);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
        [](unsigned char character) {
            return character < 0x80
                ? static_cast<char>(std::tolower(character))
                : static_cast<char>(character);
        });
    return normalized;
}

Error validate(const Envelope& envelope, const ValidationLimits& limits) {
    if (envelope.version != VERSION) return Error::UnsupportedVersion;
    if (envelope.room) {
        if (envelope.room->empty() || envelope.room->size() > limits.max_room_bytes) {
            return Error::ConstraintViolation;
        }
        if (!is_utf8(*envelope.room)) return Error::InvalidUtf8;
    }
    if (envelope.nickname) {
        if (envelope.nickname->empty() ||
            envelope.nickname->size() > limits.max_nick_bytes) {
            return Error::ConstraintViolation;
        }
        if (!is_utf8(*envelope.nickname)) return Error::InvalidUtf8;
    }
    if (is_room_type(envelope.type) && !envelope.room) return Error::MissingField;
    if (envelope.body.kind == BodyKind::Text) {
        if (envelope.body.text.size() > limits.max_body_bytes) {
            return Error::ConstraintViolation;
        }
        if (!is_utf8(envelope.body.text)) return Error::InvalidUtf8;
    }
    if (envelope.body.kind == BodyKind::Bytes &&
        envelope.body.bytes.size() > limits.max_body_bytes) {
        return Error::ConstraintViolation;
    }
    if (envelope.body.kind == BodyKind::Members &&
        envelope.body.members.size() > limits.max_members) {
        return Error::ConstraintViolation;
    }
    auto validate_optional_text = [](const std::optional<std::string>& text,
                                     size_t maximum) -> Error {
        if (!text) return Error::None;
        if (text->empty() || text->size() > maximum) {
            return Error::ConstraintViolation;
        }
        return is_utf8(*text) ? Error::None : Error::InvalidUtf8;
    };
    if (envelope.body.kind == BodyKind::Hello) {
        Error error = validate_optional_text(envelope.body.hello.client_name,
                                             MAX_CLIENT_NAME_BYTES);
        if (error != Error::None) return error;
        error = validate_optional_text(envelope.body.hello.client_version,
                                       MAX_VERSION_BYTES);
        if (error != Error::None) return error;
    }
    if (envelope.body.kind == BodyKind::Welcome) {
        Error error = validate_optional_text(envelope.body.welcome.hub_name,
                                             MAX_CLIENT_NAME_BYTES);
        if (error != Error::None) return error;
        error = validate_optional_text(envelope.body.welcome.hub_version,
                                       MAX_VERSION_BYTES);
        if (error != Error::None) return error;
    }
    if (envelope.type == T_HELLO && envelope.body.kind != BodyKind::Hello &&
        envelope.body.kind != BodyKind::None) return Error::WrongType;
    if (envelope.type == T_WELCOME && envelope.body.kind != BodyKind::Welcome) {
        return Error::WrongType;
    }
    if ((envelope.type == T_JOIN || envelope.type == T_PART) &&
        envelope.body.kind != BodyKind::None) return Error::WrongType;
    if ((envelope.type == T_JOINED || envelope.type == T_PARTED) &&
        envelope.body.kind != BodyKind::None &&
        envelope.body.kind != BodyKind::Members) return Error::WrongType;
    if (is_text_type(envelope.type) && envelope.body.kind != BodyKind::Text) {
        return Error::WrongType;
    }
    if ((envelope.type == T_PING || envelope.type == T_PONG) &&
        envelope.body.kind != BodyKind::None &&
        envelope.body.kind != BodyKind::Unsigned &&
        envelope.body.kind != BodyKind::Text &&
        envelope.body.kind != BodyKind::Bytes) return Error::WrongType;
    return Error::None;
}

Error stamp_forwarded(Envelope& envelope,
                      const IdentityHash& authenticated_source,
                      const std::optional<std::string>& accepted_nickname,
                      const ValidationLimits& limits) {
    envelope.source = authenticated_source;
    if (envelope.room) envelope.room = normalize_room(*envelope.room);
    envelope.nickname = accepted_nickname;
    return validate(envelope, limits);
}

Result encode(const Envelope& envelope, uint8_t* output, size_t capacity,
              const ValidationLimits& limits) {
    Error validation = validate(envelope, limits);
    if (validation != Error::None) return {validation, 0};
    if (output == nullptr || capacity == 0) return {Error::OutputTooSmall, 0};
    capacity = std::min(capacity, limits.max_envelope_bytes);

    size_t fields = 5 + (envelope.room ? 1 : 0) +
        (envelope.body.kind != BodyKind::None ? 1 : 0) +
        (envelope.nickname ? 1 : 0);
    CborEncoder encoder;
    CborEncoder map;
    cbor_encoder_init(&encoder, output, capacity, 0);
    CborError error = cbor_encoder_create_map(&encoder, &map, fields);
    if (error != CborNoError) return {cbor_error(error), 0};

    auto encode_key = [&](uint64_t key) -> CborError {
        return cbor_encode_uint(&map, key);
    };
    if ((error = encode_key(K_V)) != CborNoError ||
        (error = cbor_encode_uint(&map, envelope.version)) != CborNoError ||
        (error = encode_key(K_T)) != CborNoError ||
        (error = cbor_encode_uint(&map, envelope.type)) != CborNoError ||
        (error = encode_key(K_ID)) != CborNoError ||
        (error = cbor_encode_byte_string(&map, envelope.message_id.data(),
            envelope.message_id.size())) != CborNoError ||
        (error = encode_key(K_TS)) != CborNoError ||
        (error = cbor_encode_uint(&map, envelope.timestamp_ms)) != CborNoError ||
        (error = encode_key(K_SRC)) != CborNoError ||
        (error = cbor_encode_byte_string(&map, envelope.source.data(),
            envelope.source.size())) != CborNoError) {
        return {cbor_error(error), 0};
    }
    if (envelope.room) {
        if ((error = encode_key(K_ROOM)) != CborNoError ||
            (error = cbor_encode_text_string(&map, envelope.room->data(),
                envelope.room->size())) != CborNoError) {
            return {cbor_error(error), 0};
        }
    }
    if (envelope.body.kind != BodyKind::None) {
        if ((error = encode_key(K_BODY)) != CborNoError ||
            (error = encode_body(&map, envelope.body)) != CborNoError) {
            return {cbor_error(error), 0};
        }
    }
    if (envelope.nickname) {
        if ((error = encode_key(K_NICK)) != CborNoError ||
            (error = cbor_encode_text_string(&map, envelope.nickname->data(),
                envelope.nickname->size())) != CborNoError) {
            return {cbor_error(error), 0};
        }
    }
    error = cbor_encoder_close_container_checked(&encoder, &map);
    if (error != CborNoError) return {cbor_error(error), 0};
    const size_t size = cbor_encoder_get_buffer_size(&encoder, output);
    if (size > limits.max_envelope_bytes) return {Error::TooLarge, 0};
    return {Error::None, size};
}

Result decode(const uint8_t* data, size_t size, Envelope& envelope,
              const ValidationLimits& limits) {
    if (data == nullptr || size == 0) return {Error::Empty, 0};
    if (size > limits.max_envelope_bytes) return {Error::TooLarge, 0};

    CborParser parser;
    CborValue root;
    CborError cerror = cbor_parser_init(data, size, 0, &parser, &root);
    if (cerror != CborNoError || !cbor_value_is_map(&root)) {
        return {cerror == CborNoError ? Error::WrongType : cbor_error(cerror), 0};
    }
    CborValue definite = root;
    Error error = require_definite_lengths(definite);
    if (error != Error::None) return {error, 0};
    cerror = cbor_value_validate(&root,
        CborValidateCompleteData | CborValidateUtf8);
    if (cerror != CborNoError) return {cbor_error(cerror), 0};

    size_t pair_count = 0;
    if (cbor_value_get_map_length(&root, &pair_count) != CborNoError ||
        pair_count > 24) return {Error::ConstraintViolation, 0};

    uint8_t type = 0;
    error = find_type(root, type);
    if (error != Error::None) return {error, 0};

    Envelope decoded;
    decoded.type = type;
    CborValue parent = root;
    CborValue map;
    cerror = cbor_value_enter_container(&parent, &map);
    if (cerror != CborNoError) return {cbor_error(cerror), 0};
    uint16_t seen = 0;
    while (!cbor_value_at_end(&map)) {
        uint64_t key = 0;
        error = read_uint(map, key);
        if (error != Error::None) return {error, 0};
        if (key <= K_NICK) {
            const uint16_t bit = static_cast<uint16_t>(1u << key);
            if (seen & bit) return {Error::DuplicateField, 0};
            seen |= bit;
            uint64_t number = 0;
            switch (key) {
                case K_V:
                    error = read_uint(map, number);
                    if (number > UINT8_MAX) error = Error::ConstraintViolation;
                    else decoded.version = static_cast<uint8_t>(number);
                    break;
                case K_T:
                    error = read_uint(map, number);
                    if (number > UINT8_MAX) error = Error::ConstraintViolation;
                    else decoded.type = static_cast<uint8_t>(number);
                    break;
                case K_ID:
                    error = read_fixed_bytes(map, decoded.message_id);
                    break;
                case K_TS:
                    error = read_uint(map, decoded.timestamp_ms);
                    break;
                case K_SRC:
                    error = read_fixed_bytes(map, decoded.source);
                    break;
                case K_ROOM: {
                    std::string room;
                    error = read_text(map, limits.max_room_bytes, room);
                    if (error == Error::None) decoded.room = room;
                    break;
                }
                case K_BODY:
                    error = read_body(map, type, decoded.body, limits);
                    break;
                case K_NICK: {
                    std::string nickname;
                    error = read_text(map, limits.max_nick_bytes, nickname);
                    if (error == Error::None) decoded.nickname = nickname;
                    break;
                }
            }
            if (error != Error::None) return {error, 0};
        } else {
            error = skip(map);
            if (error != Error::None) return {error, 0};
        }
    }
    cerror = cbor_value_leave_container(&parent, &map);
    if (cerror != CborNoError) return {cbor_error(cerror), 0};
    constexpr uint16_t required = (1u << K_V) | (1u << K_T) | (1u << K_ID) |
        (1u << K_TS) | (1u << K_SRC);
    if ((seen & required) != required) return {Error::MissingField, 0};
    error = validate(decoded, limits);
    if (error != Error::None) return {error, 0};
    envelope = std::move(decoded);
    return {Error::None, size};
}

} // namespace RRC

#endif // RRC_PROTOCOL_CORE
