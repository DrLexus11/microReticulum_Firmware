"""Files over the mesh: ATAK's data packages and QuickPics.

PR D2. ATAK sends a file to one or more contacts in two steps, captured on the
bench 2026-09-22: it uploads the file to its streaming host's port 8443 through
the Marti sync API, keyed by SHA-256, then sends each recipient a small CoT
notice (`b-f-t-r`) naming the file, its size, its hash and a URL to fetch it
from. See *PR D2* in docs/TAKDeliveryPlan.md.

On the mesh the notice travels as it is -- addressed to whoever ATAK named --
and the file does not travel at all until the receiver asks for it:

    receiver                                  sender
      notice arrives, path measured
      fast:  FILE_REQUEST(sha256)  ------->   was this receiver sent the notice?
                                  <-------    FILE(sha256, name, bytes)  direct
      hash checked, notice handed to ATAK with the URL rewritten to itself

A slow path (LoRa) defers the request. The file itself is sent DIRECT and
never escalated to a propagation node: a node holding it would only hand it on
over whatever path the receiver has, which is the LoRa leg this avoids.

Frames, with byte zero from `tak_payload`:

    FILE_REQUEST_V1   kind(1) sha256(32)
    FILE_V1           kind(1) sha256(32) name_len(1) name(utf-8) data
"""

import hashlib
import json
import os
import re
import struct
import threading
import time
from datetime import datetime, timedelta, timezone
from urllib.parse import urlsplit

import tak_payload

HASH_BYTES = 32
MAX_NAME_BYTES = 200

# The largest file this will store or send. A data package is typically tens
# to hundreds of kilobytes, a QuickPic a few megabytes (3,008,206 bytes on the
# bench). Every receiver's LXMF router must accept this much in one transfer;
# see DELIVERY_LIMIT_KB in tak_lxmf.
MAX_FILE_BYTES = 16 * 1000 * 1000

FILESHARE_TYPE = "b-f-t-r"

# How long a rewritten notice stays current in ATAK. ATAK stamps its own at ten
# seconds, which is gone before a mesh fetch has finished.
NOTICE_STALE_SECONDS = 10 * 60

_HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256_hex(data):
    return hashlib.sha256(data).hexdigest()


def is_hash(text):
    return bool(text) and bool(_HEX64.match(text))


# ---- frames ---------------------------------------------------------------

def encode_request(file_hash):
    """Ask the sender of a notice for its file."""
    raw = bytes.fromhex(file_hash)
    if len(raw) != HASH_BYTES:
        raise ValueError("a file hash is 32 bytes")
    return bytes([tak_payload.FILE_REQUEST_V1]) + raw


def decode_request(frame):
    """The hex hash asked for, or None."""
    if (not isinstance(frame, (bytes, bytearray)) or len(frame) != 1 + HASH_BYTES
            or frame[0] != tak_payload.FILE_REQUEST_V1):
        return None
    return bytes(frame[1:]).hex()


def encode_file(file_hash, name, data):
    """One whole file, named, under the hash its notice gave."""
    name_bytes = (name or "").encode("utf-8")[:MAX_NAME_BYTES]
    return (bytes([tak_payload.FILE_V1]) + bytes.fromhex(file_hash)
            + struct.pack(">B", len(name_bytes)) + name_bytes + bytes(data))


def decode_file(frame):
    """(hex hash, name, data), or None -- including when the data does not
    match the hash it claims, which is a file that is not the one asked for."""
    if (not isinstance(frame, (bytes, bytearray)) or len(frame) < 2 + HASH_BYTES
            or frame[0] != tak_payload.FILE_V1):
        return None
    file_hash = bytes(frame[1:1 + HASH_BYTES]).hex()
    name_len = frame[1 + HASH_BYTES]
    start = 2 + HASH_BYTES + name_len
    if start > len(frame):
        return None
    name = bytes(frame[2 + HASH_BYTES:start]).decode("utf-8", "replace")
    data = bytes(frame[start:])
    if sha256_hex(data) != file_hash:
        return None
    return file_hash, name, data


# ---- ATAK's notice --------------------------------------------------------

def parse_notice(xml):
    """What a `b-f-t-r` offers, or None if this is not one.

    Returns hash, name, filename, size, sender_url, sender_uid and
    sender_callsign. Only a notice with a well-formed SHA-256 counts: the hash
    is the one thing every later step keys on.
    """
    import cot_endpoint   # late: cot_endpoint imports nothing from here
    try:
        event = cot_endpoint._parse(xml)
    except ValueError:
        return None
    if (event.get("type") or "") != FILESHARE_TYPE:
        return None
    share = event.find("detail/fileshare")
    if share is None:
        return None
    file_hash = (share.get("sha256") or "").lower()
    if not is_hash(file_hash):
        return None
    try:
        size = int(share.get("sizeInBytes") or 0)
    except ValueError:
        size = 0
    return {
        "hash": file_hash,
        "filename": share.get("filename") or "",
        "name": share.get("name") or "",
        "size": size,
        "sender_url": share.get("senderUrl") or "",
        "sender_uid": share.get("senderUid") or "",
        "sender_callsign": share.get("senderCallsign") or "",
    }


def content_url(base, file_hash):
    """Where a receiving ATAK fetches the file from this side."""
    return "%s/Marti/sync/content?hash=%s" % (base.rstrip("/"), file_hash)


def rewrite_notice(xml, url, now=None, stale_seconds=NOTICE_STALE_SECONDS):
    """The notice with its URL pointing here and a stale that outlives a fetch.

    Text substitution on the two attributes rather than a re-serialisation:
    ATAK's own output passes through otherwise unchanged, which is what keeps
    the notice ATAK's and not a reconstruction of it.
    """
    text = xml.decode("utf-8") if isinstance(xml, (bytes, bytearray)) else xml
    text = re.sub(r'senderUrl="[^"]*"', 'senderUrl="%s"' % url, text, count=1)
    now = now or datetime.now(timezone.utc)
    stamp = lambda moment: moment.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (moment.microsecond // 1000)
    text = re.sub(r'\btime="[^"]*"', 'time="%s"' % stamp(now), text, count=1)
    text = re.sub(r'\bstart="[^"]*"', 'start="%s"' % stamp(now), text, count=1)
    text = re.sub(r'\bstale="[^"]*"', 'stale="%s"' % stamp(now + timedelta(seconds=stale_seconds)),
                  text, count=1)
    return text


def url_host(url):
    try:
        return urlsplit(url).hostname
    except ValueError:
        return None


# ---- the store ------------------------------------------------------------

class FileStore:
    """Files by SHA-256, on disk, owner-only.

    Each file is `<hash>` with `<hash>.json` beside it for its name and who may
    fetch it. A file is only ever stored under the hash of its own bytes, so
    one that arrives corrupted, or is not the one asked for, is refused rather
    than stored under a name it does not deserve.
    """

    def __init__(self, root):
        self.root = root
        os.makedirs(root, mode=0o700, exist_ok=True)
        self._lock = threading.Lock()

    def _path(self, file_hash):
        if not is_hash(file_hash):
            raise ValueError("not a file hash: %r" % file_hash)
        return os.path.join(self.root, file_hash)

    def has(self, file_hash):
        try:
            return os.path.isfile(self._path(file_hash))
        except ValueError:
            return False

    def put(self, data, filename="", expected_hash=None, **meta):
        """Store bytes; the hash, or None if they are not what was expected."""
        if len(data) > MAX_FILE_BYTES:
            return None
        file_hash = sha256_hex(data)
        if expected_hash and expected_hash.lower() != file_hash:
            return None
        path = self._path(file_hash)
        with self._lock:
            if not os.path.isfile(path):
                temp = path + ".part"
                fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
                with os.fdopen(fd, "wb") as out:
                    out.write(data)
                os.replace(temp, path)
            record = self.meta(file_hash)
            record.update({k: v for k, v in meta.items() if v is not None})
            if filename:
                record["filename"] = filename
            record.setdefault("stored_at", time.time())
            record["size"] = len(data)
            self._write_meta(file_hash, record)
        return file_hash

    def read(self, file_hash):
        try:
            with open(self._path(file_hash), "rb") as source:
                return source.read()
        except (OSError, ValueError):
            return None

    def meta(self, file_hash):
        try:
            with open(self._path(file_hash) + ".json", encoding="utf-8") as source:
                return json.load(source)
        except (OSError, ValueError):
            return {}

    def _write_meta(self, file_hash, record):
        path = self._path(file_hash) + ".json"
        temp = path + ".part"
        fd = os.open(temp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8") as out:
            json.dump(record, out)
        os.replace(temp, path)

    def grant(self, file_hash, members):
        """Record who a notice for this file went to: member hashes, or None
        for the whole team. Grants accumulate -- the same file shared with A
        and then with B may be fetched by both."""
        with self._lock:
            record = self.meta(file_hash)
            if members is None:
                record["team"] = True
            else:
                granted = set(record.get("granted", []))
                granted.update(member.hex() for member in members)
                record["granted"] = sorted(granted)
            self._write_meta(file_hash, record)

    def may_fetch(self, file_hash, member, is_team_member):
        """Whether `member` was sent a notice for this file."""
        record = self.meta(file_hash)
        if member.hex() in record.get("granted", []):
            return True
        return bool(record.get("team")) and is_team_member(member)
