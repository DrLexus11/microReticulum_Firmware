"""Canonical, recipient-bound TAK tasks and acknowledgments. See TAKTasking.md."""

import struct
from dataclasses import dataclass

DOMAIN = b"urtn-tak-task-v1\0"
VERSION = 1
GOTO, STATUS = 1, 2
RECEIVED, ACCEPTED, DECLINED = 1, 2, 3
MAX_TEXT = 64
MAX_LIFETIME = 3600
FUTURE_SKEW = 30
HEADER = struct.Struct(">BB16s16s16sII")
MAX_PACKET = HEADER.size + 9 + MAX_TEXT + 64
APP = "rnstransport"
ASPECTS = ("tak", "task")


@dataclass(frozen=True)
class Message:
    kind: int
    issuer: bytes
    recipient: bytes
    task_id: bytes
    issued: int
    expires: int
    lat_e7: int = 0
    lon_e7: int = 0
    text: str = ""
    status: int = 0


def body(message):
    m = message
    if any(len(v) != 16 for v in (m.issuer, m.recipient, m.task_id)):
        raise ValueError("hashes and task IDs must be 16 bytes")
    if not 0 < m.issued < m.expires <= 0xffffffff or m.expires - m.issued > MAX_LIFETIME:
        raise ValueError("invalid task lifetime")
    wire = HEADER.pack(VERSION, m.kind, m.issuer, m.recipient,
                       m.task_id, m.issued, m.expires)
    if m.kind == GOTO:
        text = m.text.encode("utf-8", errors="strict")
        if not 1 <= len(text) <= MAX_TEXT or any(ord(c) < 32 or ord(c) == 127 for c in m.text):
            raise ValueError("instruction must be 1..64 UTF-8 bytes without control characters")
        if not -900000000 <= m.lat_e7 <= 900000000 or not -1800000000 <= m.lon_e7 <= 1800000000:
            raise ValueError("invalid coordinates")
        wire += struct.pack(">iiB", m.lat_e7, m.lon_e7, len(text)) + text
    elif m.kind == STATUS and m.status in (RECEIVED, ACCEPTED, DECLINED):
        wire += bytes([m.status])
    else:
        raise ValueError("unknown task kind or status")
    return wire


def encode(message, identity):
    if identity.hash != message.issuer:
        raise ValueError("issuer does not match signing identity")
    payload = body(message)
    signature = identity.sign(DOMAIN + payload)
    if len(signature) != 64:
        raise ValueError("invalid signature length")
    return payload + signature


def verify(wire, identity, recipient, now):
    """identity must come from explicit trust configuration, never from the packet."""
    if not HEADER.size + 1 + 64 <= len(wire) <= MAX_PACKET:
        raise ValueError("invalid packet length")
    payload, signature = wire[:-64], wire[-64:]
    version, kind, issuer, target, task_id, issued, expires = HEADER.unpack_from(payload)
    if version != VERSION or issuer != identity.hash or target != recipient:
        raise ValueError("wrong version, issuer, or recipient")
    if not identity.validate(signature, DOMAIN + payload):
        raise ValueError("invalid signature")
    if issued > now + FUTURE_SKEW or expires <= now:
        raise ValueError("future or expired task")
    fields = dict(kind=kind, issuer=issuer, recipient=target, task_id=task_id,
                  issued=issued, expires=expires)
    data = payload[HEADER.size:]
    if kind == GOTO and len(data) >= 9:
        lat, lon, length = struct.unpack_from(">iiB", data)
        if len(data) != 9 + length:
            raise ValueError("invalid instruction length")
        message = Message(**fields, lat_e7=lat, lon_e7=lon,
                          text=data[9:].decode("utf-8", errors="strict"))
    elif kind == STATUS and len(data) == 1:
        message = Message(**fields, status=data[0])
    else:
        raise ValueError("invalid task body")
    if body(message) != payload:
        raise ValueError("noncanonical task")
    return message
