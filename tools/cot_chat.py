"""GeoChat and its receipts, as tens of bytes rather than hundreds.

The last of the 853 observed events without a typed codec: 6 chat messages, 4
read receipts and 4 delivery receipts, measured at 991, 842 and 837 bytes
average. Small in number and large in size -- a single chat line with both
receipts is 2670 bytes, which TAKNative.md measures at 2.1 seconds of LoRa.

What actually has to travel is very little of that. Taking a real message from
the lab's OpenTAKServer:

    uid    GeoChat.ANDROID-7819….S-1-5-21-….3e80bd07-fdd8-4c2d-aaf4-8119b1f23a56
    __chat chatroom, id, messageId, parent, senderCallsign
    chatgrp uid0, uid1
    link   the sender again
    remarks the actual words: "where u at"
    point  the sender's position
    __serverdestination  10.121.32.111:4242:tcp:…

The uid is three identifiers concatenated, every one of which appears again
inside `detail`. The sender appears four times. The point duplicates a position
report we already send on its own schedule and cadence. The server destination
is an artefact of having had a server. Of roughly a thousand bytes, the message
is ten of them.

So this carries the message id, who it is for, and the words, and the receiving
end rebuilds an event ATAK accepts. A receipt carries no words at all.
"""

import struct
import uuid

import tak_payload

VERSION = tak_payload.CHAT_V1

KIND_MESSAGE = 0
KIND_DELIVERED = 1
KIND_READ = 2

# The CoT type each kind rebuilds as.
COT_TYPES = {
    KIND_MESSAGE: "b-t-f",
    KIND_DELIVERED: "b-t-f-d",
    KIND_READ: "b-t-f-r",
}

# ATAK's message ids are UUIDs, so sixteen bytes carries one exactly rather than
# the thirty-six the text form costs. A receipt is mostly this field, and
# halving it halves a receipt.
MESSAGE_ID_BYTES = 16

MAX_ROOM = 64
MAX_RECIPIENT = 64
MAX_TEXT = 900

# version, kind, sender, sent_unix, message id, room len, recipient len, text len
_HEADER = struct.Struct(">BBII%dsBBH" % MESSAGE_ID_BYTES)
HEADER_BYTES = _HEADER.size


def encode(kind, sender_id, message_id, room, text="", recipient="", sent_unix=0):
    """Pack a chat message or a receipt.

    `message_id` is accepted as a UUID string or as sixteen raw bytes. ATAK
    always produces a UUID; accepting bytes is what lets a receipt be built
    from a decoded message without a round trip through text.

    `recipient` is the uid this line is addressed to, empty for a room. It is
    what stops a private message being delivered to the whole team -- the room
    name alone cannot distinguish "everyone" from "one person", because ATAK
    puts the recipient's *callsign* in the room field for a direct message.

    `sent_unix` is when the author sent it, not when it was relayed. A backlog
    replayed to somebody who was away is worth nothing if every line in it is
    stamped with the moment it was replayed: the conversation arrives in the
    right order and at the wrong time, which is harder to read than no history
    at all.
    """
    if kind not in COT_TYPES:
        raise ValueError("unknown chat kind %r" % kind)
    if kind != KIND_MESSAGE and text:
        raise ValueError("a receipt carries no text")
    raw_id = _message_id_bytes(message_id)
    room_bytes = room.encode("utf-8")
    recipient_bytes = recipient.encode("utf-8")
    text_bytes = text.encode("utf-8")
    if not room_bytes or len(room_bytes) > MAX_ROOM:
        raise ValueError("chatroom must be 1..%d UTF-8 bytes" % MAX_ROOM)
    if len(recipient_bytes) > MAX_RECIPIENT:
        raise ValueError("recipient is longer than %d bytes" % MAX_RECIPIENT)
    if len(text_bytes) > MAX_TEXT:
        # Longer than this is not a chat line, and tier 2 already carries
        # anything this codec will not.
        raise ValueError("chat text is longer than %d bytes" % MAX_TEXT)
    if kind == KIND_MESSAGE and not text_bytes:
        raise ValueError("a chat message with no text is not a message")
    return (_HEADER.pack(VERSION, kind, sender_id, int(sent_unix) & 0xFFFFFFFF, raw_id,
                         len(room_bytes), len(recipient_bytes), len(text_bytes))
            + room_bytes + recipient_bytes + text_bytes)


def decode(frame):
    """Unpack a chat frame, or None for anything that is not one.

    None rather than an exception: this parses bytes off a radio, where a frame
    that is not ours is an ordinary event and not a fault.
    """
    if not isinstance(frame, (bytes, bytearray)) or len(frame) < HEADER_BYTES:
        return None
    frame = bytes(frame)
    (version, kind, sender_id, sent_unix, raw_id,
     room_length, recipient_length, text_length) = _HEADER.unpack(frame[:HEADER_BYTES])
    if version != VERSION or kind not in COT_TYPES:
        return None
    if room_length < 1 or room_length > MAX_ROOM:
        return None
    if recipient_length > MAX_RECIPIENT or text_length > MAX_TEXT:
        return None
    body = frame[HEADER_BYTES:]
    # The lengths describe the whole body. Trailing bytes mean this is not the
    # frame it claims to be.
    if len(body) != room_length + recipient_length + text_length:
        return None
    if (kind == KIND_MESSAGE) != (text_length > 0):
        # A message with no words, or a receipt carrying some. Either way it is
        # not what its own kind says it is.
        return None
    try:
        room = body[:room_length].decode("utf-8", errors="strict")
        recipient = body[room_length:room_length + recipient_length].decode(
            "utf-8", errors="strict")
        text = body[room_length + recipient_length:].decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    return {
        "kind": kind,
        "sender_id": sender_id,
        "sent_unix": sent_unix,
        "message_id": str(uuid.UUID(bytes=raw_id)),
        "room": room,
        "recipient": recipient,
        "text": text,
    }


def _message_id_bytes(message_id):
    if isinstance(message_id, (bytes, bytearray)):
        if len(message_id) != MESSAGE_ID_BYTES:
            raise ValueError("a raw message id must be %d bytes" % MESSAGE_ID_BYTES)
        return bytes(message_id)
    try:
        return uuid.UUID(str(message_id)).bytes
    except ValueError as error:
        raise ValueError("message id must be a UUID: %s" % error) from error


def chat_from_cot(cot_xml, sender_id):
    """A chat frame from a CoT event, or None if it is not chat.

    Everything discarded here is either a copy of something else in the same
    event or a copy of something we already send: the sender appears four
    times, the point duplicates a position report with its own cadence, and
    `__serverdestination` is an artefact of having had a server.
    """
    from cot_endpoint import _parse
    try:
        event = _parse(cot_xml)
    except ValueError:
        return None
    kind = _kind_for_type(event.get("type") or "")
    if kind is None:
        return None
    chat = event.find("detail/__chat")
    if chat is None:
        chat = event.find("detail/__chatreceipt")
    if chat is None:
        return None
    message_id = chat.get("messageId")
    room = chat.get("chatroom")
    if not message_id or not room:
        return None
    # Who this is actually for. ATAK puts the recipient's *callsign* in the
    # chatroom field for a direct message, so the room alone cannot tell
    # "everyone" from "one person" -- and treating every line as a broadcast
    # delivers private messages to the whole team.
    recipient = chat.get("id") or ""
    group = chat.find("chatgrp")
    if group is not None:
        if group.get("uid2"):
            # chatgrp enumerates participants, so a uid2 means three or more
            # of them: a room, and no one person this line is for. Reading
            # uid1 as a recipient there would narrow a room conversation to
            # whoever happened to be listed second.
            recipient = ""
        elif not recipient:
            recipient = group.get("uid1") or ""
    if recipient in _BROADCAST_IDS or recipient == room:
        recipient = ""
    text = ""
    if kind == KIND_MESSAGE:
        remarks = event.find("detail/remarks")
        text = (remarks.text or "") if remarks is not None else ""
        if not text:
            return None
    try:
        return encode(kind, sender_id, message_id, room, text, recipient,
                      _sent_unix(event))
    except ValueError:
        # A room or a line longer than this codec carries. Tier 2 takes it
        # instead, uncompressed into whatever it costs, rather than this
        # silently truncating somebody's words.
        return None


def build_chat_cot(decoded, sender_uid, callsign, when):
    """Rebuild an event ATAK accepts from a decoded chat frame.

    `when` is an ISO-8601 CoT timestamp, produced by the caller so that this
    stays a pure function of its inputs and the tests do not have to freeze a
    clock to assert what it emits.
    """
    kind = decoded["kind"]
    message_id = decoded["message_id"]
    sender = _escape(sender_uid)
    # The chatroom names *the other party*, which is not the same string on
    # both sides of a direct message. The author wrote their recipient's
    # callsign there; replaying that verbatim gives the recipient a thread
    # named after themselves, with the sender's name buried inside it.
    # Observed on hardware 2026-09-12: a line from DECK opened a conversation
    # headed COLUMBA on COLUMBA's own device.
    #
    # So a direct message is re-headed with the sender's callsign on the way
    # in. A room line keeps its room, because there the room really is the
    # same string for everybody.
    room = _escape(callsign if decoded.get("recipient") else decoded["room"])
    # A message's uid is three identifiers concatenated, which is how ATAK
    # threads a conversation; a receipt's uid is the id of the message it is
    # about, which is how ATAK matches it to the line on screen.
    # The recipient as it will appear to ATAK: the peer's own uid for a direct
    # message, the room for a broadcast. Threading depends on this being a uid
    # and not a callsign, which is what it used to be.
    target = _escape(decoded.get("recipient") or "") or room
    uid = ("GeoChat.%s.%s.%s" % (sender, target, message_id) if kind == KIND_MESSAGE
           else message_id)
    element = "__chat" if kind == KIND_MESSAGE else "__chatreceipt"
    body = (
        '<%s chatroom="%s" groupOwner="false" id="%s" messageId="%s" '
        'parent="RootContactGroup" senderCallsign="%s">'
        '<chatgrp id="%s" uid0="%s" uid1="%s"/></%s>'
        % (element, room, target, message_id, _escape(callsign),
           target, sender, target, element)
    )
    body += '<link relation="p-p" type="a-f-G-U-C" uid="%s"/>' % sender
    if kind == KIND_MESSAGE:
        # The author's time, not the relay's. This is what makes a replayed
        # conversation read in the order it happened.
        sent = decoded.get("sent_unix") or 0
        stamp = _iso(sent) if sent else when
        body += ('<remarks source="BAO.F.ATAK.%s" time="%s" to="%s">%s</remarks>'
                 % (sender, stamp, target, _escape(decoded["text"])))
    body += '<marti><dest callsign="%s"/></marti>' % room
    return ('<event version="2.0" uid="%s" type="%s" how="h-g-i-g-o" '
            'time="%s" start="%s" stale="%s">'
            '<point lat="0.0" lon="0.0" hae="9999999.0" ce="9999999.0" le="9999999.0"/>'
            '<detail>%s</detail></event>'
            % (_escape(uid), COT_TYPES[kind], when, when, when, body))


# What ATAK calls the everyone-room. A line addressed to one of these is a
# broadcast and has no single recipient.
_BROADCAST_IDS = ("All Chat Rooms", "All Streaming", "RootContactGroup", "")


def _sent_unix(event):
    """When the author sent this, from the event, or 0 if it does not say.

    Zero rather than now: a receiver can tell "no time given" from a time, and
    stamping the relay moment here is what would make a replayed backlog look
    like it all happened at once.
    """
    from datetime import datetime
    remarks = event.find("detail/remarks")
    stamp = (remarks.get("time") if remarks is not None else None) or event.get("time")
    if not stamp:
        return 0
    try:
        return int(datetime.fromisoformat(stamp.replace("Z", "+00:00")).timestamp())
    except ValueError:
        return 0


def _kind_for_type(cot_type):
    for kind, name in COT_TYPES.items():
        if cot_type == name:
            return kind
    return None


def _iso(unix_seconds):
    """A CoT timestamp from unix seconds, in the form ATAK writes."""
    from datetime import datetime, timezone
    return (datetime.fromtimestamp(unix_seconds, tz=timezone.utc)
            .strftime("%Y-%m-%dT%H:%M:%S.%f")[:-3] + "Z")


def _escape(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))
