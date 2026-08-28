#include "RRCBridge.h"

#if defined(RRC_LXMF_BRIDGE)

#include <array>
#include <cstdio>
#include <cstring>

#include "LXMFCompose.h"
#include "LXMFPropagation.h"

bool rrc_bridge_enabled = false;
char rrc_bridge_rooms[128] = "";

namespace {

constexpr size_t PUBLIC_KEY_BYTES = 64;   // encryption key + signing key

struct Member {
    bool used = false;
    RRC::IdentityHash hash{};
    RNS::Bytes public_key;
};

struct Room {
    bool used = false;
    std::string name;
    std::array<Member, RRC_BRIDGE_MAX_MEMBERS> members{};
};

struct Pending {
    bool used = false;
    uint8_t room = 0;
    double timestamp = 0;
    std::string text;
    std::array<RRC::IdentityHash, RRC_BRIDGE_MAX_MEMBERS> recipients{};
    uint8_t recipient_count = 0;
    uint8_t next = 0;
};

std::array<Room, RRC_BRIDGE_MAX_ROOMS> rooms{};
std::array<Pending, RRC_BRIDGE_QUEUE_DEPTH> queue{};

RNS::Identity hub_identity{RNS::Type::NONE};
RNS::Bytes hub_delivery_hash;
bool running = false;

// Transient ids of messages this node composed, oldest first, so the quota can
// evict our own before the store's cap starts evicting anyone's.
std::vector<RNS::Bytes> bridged_ids;

uint32_t delivered_count = 0;
uint32_t dropped_count = 0;

// The roster is the only part of the bridge that must outlive a reboot. Without
// it a restarted hub delivers nothing to anyone until every member has joined
// again, and presents exactly like a healthy idle bridge while doing so.
bool roster_dirty = false;
uint64_t roster_saved_ms = 0;

// Joins are infrequent but arrive in bursts when a net forms, so writes are
// debounced rather than done per join. Flash is the scarce resource here.
#define RRC_BRIDGE_ROSTER_PATH  "/rrc_roster"
#define RRC_BRIDGE_ROSTER_MAGIC 0x52
#define RRC_BRIDGE_ROSTER_VERSION 1
#define RRC_BRIDGE_ROSTER_SAVE_MS 30000

int find_room(const std::string& name) {
    for (size_t i = 0; i < rooms.size(); i++) {
        if (rooms[i].used && rooms[i].name == name) return (int)i;
    }
    return -1;
}

// Parse the provisioned comma-separated room list. Names are normalized the
// same way RRC normalizes them everywhere else, so "#general", "general" and
// "General" all name one room.
void parse_rooms() {
    for (auto& room : rooms) { room.used = false; room.name.clear(); }

    const std::string list(rrc_bridge_rooms);
    size_t start = 0;
    size_t slot = 0;
    while (start <= list.size() && slot < rooms.size()) {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos) comma = list.size();
        std::string token = list.substr(start, comma - start);
        start = comma + 1;

        // Trim; a provisioned list is hand-entered and will contain spaces.
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t')) {
            token.erase(token.begin());
        }
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t')) {
            token.pop_back();
        }
        if (token.empty()) { if (comma >= list.size()) break; continue; }

        const std::string name = RRC::normalize_room(token);
        if (name.empty() || find_room(name) >= 0) {
            if (comma >= list.size()) break;
            continue;
        }
        rooms[slot].used = true;
        rooms[slot].name = name;
        slot++;
        if (comma >= list.size()) break;
    }
}

void roster_save() {
    RNS::Bytes out;
    out.append((uint8_t)RRC_BRIDGE_ROSTER_MAGIC);
    out.append((uint8_t)RRC_BRIDGE_ROSTER_VERSION);
    for (const auto& room : rooms) {
        if (!room.used || room.name.size() > 255) continue;
        size_t count = 0;
        for (const auto& member : room.members) if (member.used) count++;
        if (count == 0) continue;

        out.append((uint8_t)room.name.size());
        out.append(room.name);
        out.append((uint8_t)count);
        for (const auto& member : room.members) {
            if (!member.used) continue;
            out.append(member.hash.data(), member.hash.size());
            out.append(member.public_key);
        }
    }
    if (RNS::Utilities::OS::write_file(RRC_BRIDGE_ROSTER_PATH, out) != out.size()) {
        printf("[rrc] FAILED to save the bridge roster\n");
        return;
    }
    roster_dirty = false;
    roster_saved_ms = RNS::Utilities::OS::ltime();
}

void roster_load() {
    RNS::Bytes in;
    if (RNS::Utilities::OS::read_file(RRC_BRIDGE_ROSTER_PATH, in) < 2) return;
    if (in.data()[0] != RRC_BRIDGE_ROSTER_MAGIC ||
        in.data()[1] != RRC_BRIDGE_ROSTER_VERSION) {
        printf("[rrc] ignoring a bridge roster written by another version\n");
        return;
    }

    size_t at = 2;
    while (at < in.size()) {
        const size_t name_len = in.data()[at++];
        if (at + name_len + 1 > in.size()) break;
        const std::string name((const char*)in.data() + at, name_len);
        at += name_len;
        size_t count = in.data()[at++];

        // A room dropped from the provisioned list keeps its stored roster but
        // is skipped here, so re-adding it later does not start from nothing.
        const int index = find_room(name);
        for (size_t i = 0; i < count; i++) {
            const size_t entry = RRC::IDENTITY_HASH_BYTES + PUBLIC_KEY_BYTES;
            if (at + entry > in.size()) return;
            if (index >= 0) {
                for (auto& slot : rooms[index].members) {
                    if (slot.used) continue;
                    slot.used = true;
                    std::memcpy(slot.hash.data(), in.data() + at,
                                RRC::IDENTITY_HASH_BYTES);
                    slot.public_key = RNS::Bytes(
                        in.data() + at + RRC::IDENTITY_HASH_BYTES, PUBLIC_KEY_BYTES);
                    break;
                }
            }
            at += entry;
        }
    }
    printf("[rrc] bridge roster loaded: %u member(s)\n",
           (unsigned)rrc_bridge_member_count());
}

bool same_identity(const RRC::IdentityHash& a, const RRC::IdentityHash& b) {
    return std::memcmp(a.data(), b.data(), a.size()) == 0;
}

// Compose and store one message for one recipient. Returns false when the
// recipient cannot be addressed or the store refused it -- both are counted as
// drops rather than retried, because a retry would repeat the same failure and
// the queue has to keep moving.
bool deliver(const Room& room, const Member& member, const Pending& pending) {
    RNS::Identity recipient(false);
    recipient.load_public_key(member.public_key);
    recipient.update_hashes();
    if (!recipient) return false;

    RNS::Destination destination(recipient, RNS::Type::Destination::OUT,
                                 RNS::Type::Destination::SINGLE,
                                 LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);

    // The room name is the subject, so a stock client shows bridged traffic as
    // a conversation per room rather than one undifferentiated thread.
    const RNS::Bytes title(room.name);
    const RNS::Bytes content(pending.text);

    const RNS::Bytes blob = lxmf_compose_propagated(
        hub_identity, hub_delivery_hash, destination,
        pending.timestamp, title, content);
    if (!blob) return false;

    // Keep bridged traffic inside its share of the store before the store's own
    // cap gets a chance to evict a resident's mail to make room for it.
    while (bridged_ids.size() >= (size_t)RRC_BRIDGE_STORE_QUOTA &&
           !bridged_ids.empty()) {
        lxmf_store_remove(bridged_ids.front());
        bridged_ids.erase(bridged_ids.begin());
    }

    if (!lxmf_store_put(blob)) return false;
    bridged_ids.push_back(lxmf_transient_id(blob));
    return true;
}

} // namespace

void rrc_bridge_begin(const RNS::Identity& identity) {
    running = false;
    if (!rrc_bridge_enabled || !identity) return;

    hub_identity = identity;
    RNS::Destination delivery(identity, RNS::Type::Destination::OUT,
                              RNS::Type::Destination::SINGLE,
                              LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
    hub_delivery_hash = delivery.hash();
    if (hub_delivery_hash.size() != LXMF_DESTINATION_LEN) return;

    parse_rooms();
    roster_load();
    running = true;
    printf("[rrc] bridge enabled for %u room(s), source <%s>\n",
           (unsigned)rrc_bridge_room_count(),
           hub_delivery_hash.toHex().c_str());
}

bool rrc_bridge_bridged(const std::string& room) {
    if (!running) return false;
    return find_room(room) >= 0;
}

void rrc_bridge_remember(const std::string& room, const RNS::Identity& member) {
    if (!running || !member) return;
    const int index = find_room(room);
    if (index < 0) return;

    const RNS::Bytes public_key = member.get_public_key();
    if (public_key.size() != PUBLIC_KEY_BYTES) return;

    RRC::IdentityHash hash{};
    if (member.hash().size() != hash.size()) return;
    std::memcpy(hash.data(), member.hash().data(), hash.size());

    Room& target = rooms[index];
    for (auto& existing : target.members) {
        if (existing.used && same_identity(existing.hash, hash)) {
            if (existing.public_key != public_key) {
                existing.public_key = public_key;   // keys can be re-issued
                roster_dirty = true;
            }
            return;
        }
    }
    for (auto& slot : target.members) {
        if (slot.used) continue;
        slot.used = true;
        slot.hash = hash;
        slot.public_key = public_key;
        roster_dirty = true;
        return;
    }
    // Roster full. Dropping the newcomer rather than evicting an established
    // member keeps the standing membership of a command room stable, which is
    // the property that matters more than admitting the most recent arrival.
    ++dropped_count;
}

void rrc_bridge_publish(const std::string& room, const std::string& nickname,
                        const RRC::IdentityHash& sender, const std::string& text,
                        const std::vector<RRC::IdentityHash>& present) {
    if (!running || text.empty()) return;
    const int index = find_room(room);
    if (index < 0) return;

    Pending* pending = nullptr;
    for (auto& entry : queue) {
        if (entry.used) continue;
        pending = &entry;
        break;
    }
    if (!pending) {
        // The queue is depth-bounded and drains one recipient per loop, so a
        // room busier than the radio can carry will overrun it. Dropping the
        // newest keeps what is already queued moving; the alternative starves
        // deliveries that are part-done.
        ++dropped_count;
        return;
    }

    *pending = Pending{};
    pending->room = (uint8_t)index;
    pending->timestamp = (double)RNS::Utilities::OS::time();
    pending->text = nickname.empty() ? text : ("<" + nickname + "> " + text);

    for (const auto& member : rooms[index].members) {
        if (!member.used) continue;
        if (same_identity(member.hash, sender)) continue;   // never to the author

        bool connected = false;
        for (const auto& here : present) {
            if (same_identity(member.hash, here)) { connected = true; break; }
        }
        if (connected) continue;                            // got the live fanout

        if (pending->recipient_count >= pending->recipients.size()) break;
        pending->recipients[pending->recipient_count++] = member.hash;
    }

    if (pending->recipient_count == 0) return;              // nobody was away
    pending->used = true;
}

void rrc_bridge_loop() {
    if (!running) return;

    if (roster_dirty &&
        RNS::Utilities::OS::ltime() - roster_saved_ms >= RRC_BRIDGE_ROSTER_SAVE_MS) {
        roster_save();
    }

    for (auto& pending : queue) {
        if (!pending.used) continue;

        // One recipient per call. Signing and encrypting are the expensive part
        // and this runs on the same loop that services the radio.
        if (pending.next >= pending.recipient_count) { pending.used = false; continue; }

        const Room& room = rooms[pending.room];
        const RRC::IdentityHash& target = pending.recipients[pending.next++];

        const Member* member = nullptr;
        for (const auto& candidate : room.members) {
            if (candidate.used && same_identity(candidate.hash, target)) {
                member = &candidate;
                break;
            }
        }

        if (member && deliver(room, *member, pending)) ++delivered_count;
        else ++dropped_count;

        if (pending.next >= pending.recipient_count) pending.used = false;
        return;
    }
}

size_t rrc_bridge_room_count() {
    size_t count = 0;
    for (const auto& room : rooms) if (room.used) count++;
    return count;
}

size_t rrc_bridge_member_count() {
    size_t count = 0;
    for (const auto& room : rooms) {
        if (!room.used) continue;
        for (const auto& member : room.members) if (member.used) count++;
    }
    return count;
}

size_t rrc_bridge_queue_depth() {
    size_t count = 0;
    for (const auto& pending : queue) if (pending.used) count++;
    return count;
}

uint32_t rrc_bridge_delivered_count() { return delivered_count; }
uint32_t rrc_bridge_dropped_count() { return dropped_count; }

#endif // RRC_LXMF_BRIDGE
