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
MAX_TEXT = 900

_HEADER = struct.Struct(">BBI%dsBH" % MESSAGE_ID_BYTES)
HEADER_BYTES = _HEADER.size


def encode(kind, sender_id, message_id, room, text=""):
    """Pack a chat message or a receipt.

    `message_id` is accepted as a UUID string or as sixteen raw bytes. ATAK
    always produces a UUID; accepting bytes is what lets a receipt be built
    from a decoded message without a round trip through text.
    """
    if kind not in COT_TYPES:
        raise ValueError("unknown chat kind %r" % kind)
    if kind != KIND_MESSAGE and text:
        raise ValueError("a receipt carries no text")
    raw_id = _message_id_bytes(message_id)
    room_bytes = room.encode("utf-8")
    text_bytes = text.encode("utf-8")
    if not room_bytes or len(room_bytes) > MAX_ROOM:
        raise ValueError("chatroom must be 1..%d UTF-8 bytes" % MAX_ROOM)
    if len(text_bytes) > MAX_TEXT:
        # Longer than this is not a chat line, and tier 2 already carries
        # anything this codec will not.
        raise ValueError("chat text is longer than %d bytes" % MAX_TEXT)
    if kind == KIND_MESSAGE and not text_bytes:
        raise ValueError("a chat message with no text is not a message")
    return (_HEADER.pack(VERSION, kind, sender_id, raw_id,
                         len(room_bytes), len(text_bytes))
            + room_bytes + text_bytes)


def decode(frame):
    """Unpack a chat frame, or None for anything that is not one.

    None rather than an exception: this parses bytes off a radio, where a frame
    that is not ours is an ordinary event and not a fault.
    """
    if not isinstance(frame, (bytes, bytearray)) or len(frame) < HEADER_BYTES:
        return None
    frame = bytes(frame)
    version, kind, sender_id, raw_id, room_length, text_length = _HEADER.unpack(
        frame[:HEADER_BYTES])
    if version != VERSION or kind not in COT_TYPES:
        return None
    if room_length < 1 or room_length > MAX_ROOM or text_length > MAX_TEXT:
        return None
    body = frame[HEADER_BYTES:]
    # The lengths describe the whole body. Trailing bytes mean this is not the
    # frame it claims to be.
    if len(body) != room_length + text_length:
        return None
    if (kind == KIND_MESSAGE) != (text_length > 0):
        # A message with no words, or a receipt carrying some. Either way it is
        # not what its own kind says it is.
        return None
    try:
        room = body[:room_length].decode("utf-8", errors="strict")
        text = body[room_length:].decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    return {
        "kind": kind,
        "sender_id": sender_id,
        "message_id": str(uuid.UUID(bytes=raw_id)),
        "room": room,
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
    text = ""
    if kind == KIND_MESSAGE:
        remarks = event.find("detail/remarks")
        text = (remarks.text or "") if remarks is not None else ""
        if not text:
            return None
    try:
        return encode(kind, sender_id, message_id, room, text)
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
    room = _escape(decoded["room"])
    message_id = decoded["message_id"]
    sender = _escape(sender_uid)
    # A message's uid is three identifiers concatenated, which is how ATAK
    # threads a conversation; a receipt's uid is the id of the message it is
    # about, which is how ATAK matches it to the line on screen.
    uid = ("GeoChat.%s.%s.%s" % (sender, room, message_id) if kind == KIND_MESSAGE
           else message_id)
    element = "__chat" if kind == KIND_MESSAGE else "__chatreceipt"
    body = (
        '<%s chatroom="%s" groupOwner="false" id="%s" messageId="%s" '
        'parent="RootContactGroup" senderCallsign="%s">'
        '<chatgrp id="%s" uid0="%s" uid1="%s"/></%s>'
        % (element, room, room, message_id, _escape(callsign),
           room, sender, room, element)
    )
    body += '<link relation="p-p" type="a-f-G-U-C" uid="%s"/>' % sender
    if kind == KIND_MESSAGE:
        body += ('<remarks source="BAO.F.ATAK.%s" time="%s" to="%s">%s</remarks>'
                 % (sender, when, room, _escape(decoded["text"])))
    body += '<marti><dest callsign="%s"/></marti>' % room
    return ('<event version="2.0" uid="%s" type="%s" how="h-g-i-g-o" '
            'time="%s" start="%s" stale="%s">'
            '<point lat="0.0" lon="0.0" hae="9999999.0" ce="9999999.0" le="9999999.0"/>'
            '<detail>%s</detail></event>'
            % (_escape(uid), COT_TYPES[kind], when, when, when, body))


def _kind_for_type(cot_type):
    for kind, name in COT_TYPES.items():
        if cot_type == name:
            return kind
    return None


def _escape(value):
    return (value.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))
