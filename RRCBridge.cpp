#include "RRCBridge.h"

#if defined(RRC_LXMF_BRIDGE)

#include <array>
#include <cstdio>
#include <cstring>

#include "LXMFCompose.h"
#include "RRCHub.h"
#include "LXMFPropagation.h"

bool rrc_bridge_enabled = false;
char rrc_bridge_rooms[128] = "";
uint8_t rrc_bridge_history = 20;

namespace {

constexpr size_t PUBLIC_KEY_BYTES = 64;   // encryption key + signing key

struct Member {
    bool used = false;
    bool backfilled = false;   // has already been sent the catch-up for this room
    RRC::IdentityHash hash{};
    RNS::Bytes public_key;
};

// One room message, kept whole rather than pre-formatted. The readable body is
// derived from this; so is the structured metadata. Keeping the parts means a
// client can be given identity, id and original time, none of which survive
// being flattened into a display string.
struct Line {
    RRC::MessageId id{};
    RRC::IdentityHash sender{};
    uint64_t timestamp_ms = 0;
    char nick[RRC_BRIDGE_HISTORY_NICK] = {0};
    char text[RRC_BRIDGE_HISTORY_TEXT] = {0};
};

struct Room {
    bool used = false;
    std::string name;
    std::array<Member, RRC_BRIDGE_MAX_MEMBERS> members{};
    // Ring of recent lines, oldest-to-newest by (head + i) % count.
    Line* history = nullptr;          // RRC_BRIDGE_HISTORY_MAX entries
    uint8_t history_head = 0;
    uint8_t history_count = 0;
};

struct Pending {
    bool used = false;
    uint8_t room = 0;
    double timestamp = 0;
    std::string text;
    RNS::Bytes fields;          // pre-packed; built off the crypto path
    std::array<RRC::IdentityHash, RRC_BRIDGE_MAX_MEMBERS> recipients{};
    uint8_t recipient_count = 0;
    uint8_t next = 0;
};

std::array<Room, RRC_BRIDGE_MAX_ROOMS> rooms{};
std::array<Pending, RRC_BRIDGE_QUEUE_DEPTH> queue{};

RNS::Identity hub_identity{RNS::Type::NONE};
RNS::Destination hub_delivery{RNS::Type::NONE};
RNS::Bytes hub_delivery_hash;
bool running = false;
uint64_t next_announce_ms = 0;

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

// Matched with room_token_matches rather than string equality, because
// normalize_room() does not strip a leading '#' -- so a client that joins
// "#command" lands in a different room than a node provisioned with "command",
// and the bridge would simply never fire for it. Silently, which is the worst
// available outcome: the room works, people talk in it, and nobody who was
// away ever receives anything.
int find_room(const std::string& name) {
    for (size_t i = 0; i < rooms.size(); i++) {
        if (rooms[i].used && RRC::room_token_matches(rooms[i].name, name)) {
            return (int)i;
        }
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

// Announce the delivery destination the bridge sends from.
//
// Without this the messages arrive, decrypt and display correctly, and are
// still shown as unverified: LXMF validates a signature by recalling the
// source identity from an announce, and if it has never heard one it reports
// SOURCE_UNKNOWN rather than a bad signature. The two are indistinguishable in
// LXMessage.signature_validated, which is exactly how this was nearly missed
// -- the first end-to-end test showed "signature: INVALID" on a message whose
// signature was fine.
//
// Field order and types match LXMRouter.get_announce_app_data():
// msgpack [display_name, stamp_cost, [supported_functionality]].
RNS::Bytes delivery_app_data() {
	MsgPack::Packer packer;
	packer.serialize(MsgPack::arr_size_t(3));
	const std::string name(rrc_hub_name);
	if (name.empty()) packer.serialize(MsgPack::object::nil_t());
	else packer.packBinary((const uint8_t*)name.data(), name.size());
	packer.serialize(MsgPack::object::nil_t());   // no delivery stamp cost
	packer.serialize(MsgPack::arr_size_t(1));
	packer.serialize((uint8_t)0x00);              // SF_COMPRESSION
	return RNS::Bytes(packer.data(), packer.size());
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

size_t history_depth() {
    return rrc_bridge_history > RRC_BRIDGE_HISTORY_MAX
         ? (size_t)RRC_BRIDGE_HISTORY_MAX : (size_t)rrc_bridge_history;
}

void copy_bounded(char* target, size_t capacity, const std::string& value) {
    const size_t length = value.size() < (capacity - 1) ? value.size() : (capacity - 1);
    std::memcpy(target, value.data(), length);
    target[length] = 0;
}

void history_append(Room& room, const Line& line) {
    if (!room.history || history_depth() == 0) return;
    const size_t depth = history_depth();
    const size_t at = (room.history_head + room.history_count) % depth;
    room.history[at] = line;
    if (room.history_count < depth) room.history_count++;
    else room.history_head = (uint8_t)((room.history_head + 1) % depth);
}

// A selection of lines to send as one message. A live relay is a selection of
// one and a catch-up is a selection of many, so both paths below build the same
// body and the same metadata from the same code.
struct Selection {
    std::array<const Line*, RRC_BRIDGE_HISTORY_MAX> lines{};
    size_t count = 0;
};

// Newest-first under a byte budget, returned in chronological order. The budget
// keeps a deep history from composing a message larger than the store is
// required to accept; the reordering is because reading a conversation
// backwards is worse than reading less of it.
Selection history_select(Room& room) {
    Selection selection;
    if (!room.history || room.history_count == 0) return selection;
    const size_t depth = history_depth();

    size_t take = 0, budget = 0;
    for (size_t i = 0; i < room.history_count; i++) {
        const size_t index = (room.history_head + room.history_count - 1 - i) % depth;
        const size_t length = std::strlen(room.history[index].text) + 32;
        if (budget + length > RRC_BRIDGE_DIGEST_BUDGET) break;
        budget += length;
        take++;
    }
    for (size_t i = 0; i < take; i++) {
        const size_t index = (room.history_head + room.history_count - take + i) % depth;
        selection.lines[selection.count++] = &room.history[index];
    }
    return selection;
}

std::string selection_text(const Selection& selection, const std::string& label) {
    std::string out;
    for (size_t i = 0; i < selection.count; i++) {
        const Line& line = *selection.lines[i];
        if (!out.empty()) out += "\n";
        if (line.nick[0]) { out += "<"; out += line.nick; out += " / "; out += label; out += "> "; }
        else              { out += "<"; out += label; out += "> "; }
        out += line.text;
    }
    return out;
}

// Field order and types are docs/BridgeClientContract.md 3. Built here, on the
// callback path, because it is pure serialisation -- the cryptography that must
// not run here happens later, on the main loop.
RNS::Bytes selection_fields(const Room& room, const Selection& selection) {
    if (selection.count == 0) return RNS::Bytes();
    const RNS::Bytes hub = rrc_hub_destination_hash();

    MsgPack::Packer packer;
    packer.serialize(MsgPack::map_size_t(3));

    packer.serialize((uint8_t)LXMF_FIELD_CUSTOM_TYPE);
    packer.pack(RRC_BRIDGE_FIELD_TYPE);

    packer.serialize((uint8_t)LXMF_FIELD_CUSTOM_DATA);
    packer.serialize(MsgPack::map_size_t(3));
    packer.serialize((uint8_t)0);
    packer.pack(room.name.c_str());
    packer.serialize((uint8_t)1);
    packer.packBinary(hub.data(), hub.size());
    packer.serialize((uint8_t)2);
    packer.serialize(MsgPack::arr_size_t(selection.count));
    for (size_t i = 0; i < selection.count; i++) {
        const Line& line = *selection.lines[i];
        packer.serialize(MsgPack::arr_size_t(5));
        packer.packBinary(line.id.data(), line.id.size());
        packer.packBinary(line.sender.data(), line.sender.size());
        packer.pack(line.nick);
        packer.serialize((uint64_t)line.timestamp_ms);
        packer.pack(line.text);
    }

    // Lets a client tell a short history from a truncated one. Without the
    // oldest timestamp it cannot know whether a gap it sees is real, and a
    // command log that hides its own gaps is worse than one that admits them.
    packer.serialize((uint8_t)LXMF_FIELD_CUSTOM_META);
    packer.serialize(MsgPack::map_size_t(2));
    packer.serialize((uint8_t)0);
    packer.serialize((uint64_t)history_depth());
    packer.serialize((uint8_t)1);
    packer.serialize((uint64_t)(room.history_count ? room.history[room.history_head].timestamp_ms : 0));

    return RNS::Bytes(packer.data(), packer.size());
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

    // The key and the identity hash are stored as separate fields and reloaded
    // from a file that a power loss can leave partially written. They must
    // agree: the delivery destination is derived from the *key*, so a record
    // whose halves have drifted apart would encrypt a room's traffic to
    // whoever that key belongs to and address it to them, correctly, with
    // nothing anywhere reporting a fault. Misdelivering a command room to a
    // stranger is the one failure here worth a branch on every send.
    if (recipient.hash().size() != member.hash.size() ||
        std::memcmp(recipient.hash().data(), member.hash.data(),
                    member.hash.size()) != 0) {
        printf("[rrc] roster entry for <%s> does not match its key; not sending\n",
               RNS::Bytes(member.hash.data(), member.hash.size()).toHex().c_str());
        return false;
    }

    RNS::Destination destination(recipient, RNS::Type::Destination::OUT,
                                 RNS::Type::Destination::SINGLE,
                                 LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);

    // The room name is the subject, so a stock client shows bridged traffic as
    // a conversation per room rather than one undifferentiated thread.
    const RNS::Bytes title(room.name);
    const RNS::Bytes content(pending.text);

    const RNS::Bytes blob = lxmf_compose_propagated(
        hub_identity, hub_delivery_hash, destination,
        pending.timestamp, title, content, pending.fields);
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

// Queue the catch-up for a member seen in this room for the first time.
//
// Cheap enough for the Link callback this runs in: it copies text and does no
// cryptography, like every other publish path here. The composing happens on
// the main loop.
void queue_backfill(Room& room, Member& member) {
    if (member.backfilled) return;
    member.backfilled = true;          // one attempt; never a retry storm
    roster_dirty = true;

    const Selection selection = history_select(room);
    if (selection.count == 0) return;
    const std::string digest = selection_text(selection, room.name);
    if (digest.empty()) return;

    for (auto& entry : queue) {
        if (entry.used) continue;
        entry = Pending{};
        entry.room = (uint8_t)(&room - rooms.data());
        entry.timestamp = (double)RNS::Utilities::OS::time();
        entry.text = digest;
        entry.fields = selection_fields(room, selection);
        entry.recipients[0] = member.hash;
        entry.recipient_count = 1;
        entry.used = true;
        return;
    }
    // No slot free. The catch-up is a courtesy, not a delivery guarantee, and
    // dropping it is better than displacing traffic already queued for people
    // who are waiting on it.
    ++dropped_count;
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

    // Held as an IN destination so it can announce; the OUT copy above was
    // only ever needed for its hash.
    hub_delivery = RNS::Destination(identity, RNS::Type::Destination::IN,
                                    RNS::Type::Destination::SINGLE,
                                    LXMF_APP_NAME, LXMF_DELIVERY_ASPECT);
    next_announce_ms = RNS::Utilities::OS::ltime() + RRC_BRIDGE_FIRST_ANNOUNCE_MS;

    parse_rooms();

    // PSRAM, explicitly. These are bulk, non-DMA buffers and internal RAM is
    // the resource this board actually runs out of.
    for (auto& room : rooms) {
        if (!room.used || room.history) continue;
        const size_t bytes = (size_t)RRC_BRIDGE_HISTORY_MAX * sizeof(Line);
#if defined(BOARD_HAS_PSRAM)
        room.history = (Line*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
#endif
        if (!room.history) room.history = (Line*)malloc(bytes);
        if (room.history) std::memset(room.history, 0, bytes);
        else printf("[rrc] no memory for %s history; catch-up disabled\n",
                    room.name.c_str());
    }

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
            // Still a candidate for catch-up. The roster is persisted but the
            // backfilled flag is not, so after a restart every known member is
            // owed one again -- which is right, because a restart is exactly
            // when they are most likely to have missed something. Without this
            // the catch-up only ever reached a member never seen before during
            // the current uptime, and never anyone at all after a reboot.
            queue_backfill(target, existing);
            return;
        }
    }
    for (auto& slot : target.members) {
        if (slot.used) continue;
        slot.used = true;
        slot.hash = hash;
        slot.public_key = public_key;
        roster_dirty = true;
        queue_backfill(target, slot);
        return;
    }
    // Roster full. Dropping the newcomer rather than evicting an established
    // member keeps the standing membership of a command room stable, which is
    // the property that matters more than admitting the most recent arrival.
    ++dropped_count;
}

void rrc_bridge_publish(const std::string& room, const std::string& nickname,
                        const RRC::IdentityHash& sender,
                        const RRC::MessageId& message_id, uint64_t timestamp_ms,
                        const std::string& text,
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

    // Name the room in the body as well as the title. A client that shows only
    // the message line -- which is the common case -- otherwise gives a reader
    // no way to tell which room a backfilled message came from, and someone
    // catching up across several rooms sees one undifferentiated list.
    //
    // The configured room name is used rather than the token the sender typed,
    // so "#command" and "command" both read as one room. The hub is not named:
    // the message is delivered by this node and its source address already
    // says which node that was.
    Line line;
    line.id = message_id;
    line.sender = sender;
    line.timestamp_ms = timestamp_ms;
    copy_bounded(line.nick, sizeof(line.nick), nickname);
    copy_bounded(line.text, sizeof(line.text), text);

    // Recorded before the selection is built, so the message being relayed is
    // the newest entry of the catch-up anyone joining a moment later receives.
    history_append(rooms[index], line);

    Selection one;
    one.lines[0] = &line;
    one.count = 1;
    pending->text = selection_text(one, rooms[index].name);
    pending->fields = selection_fields(rooms[index], one);

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

    if (hub_delivery && RNS::Utilities::OS::ltime() >= next_announce_ms) {
        hub_delivery.announce(delivery_app_data());
        next_announce_ms = RNS::Utilities::OS::ltime() + RRC_BRIDGE_ANNOUNCE_MS;
        printf("[rrc] announced bridge delivery address <%s>\n",
               hub_delivery_hash.toHex().c_str());
    }

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

size_t rrc_bridge_history_depth() { return history_depth(); }

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
