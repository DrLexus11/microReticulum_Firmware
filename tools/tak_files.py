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


# Who a status line comes from in ATAK's chat: this node, not a teammate.
STATUS_UID = "COLUMBA-FILES"
STATUS_CALLSIGN = "Columba files"


def status_line(our_uid, text, now=None):
    """A chat line to this node's own ATAK about a file, from the bridge.

    Local only -- nothing goes on the air. ATAK cannot show "queued, waiting
    for a fast path": a receiver sees nothing and a sender sees its send time
    out as a failure, while the file is simply waiting. From its own contact
    rather than the teammate's, so nothing is put in a teammate's mouth.
    """
    import uuid
    import cot_chat
    now = now or datetime.now(timezone.utc)
    decoded = {"kind": cot_chat.KIND_MESSAGE, "sender_id": 0,
               "sent_unix": int(now.timestamp()), "message_id": str(uuid.uuid4()),
               "room": STATUS_CALLSIGN, "recipient": our_uid, "text": text}
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (now.microsecond // 1000)
    return cot_chat.build_chat_cot(decoded, STATUS_UID, STATUS_CALLSIGN, stamp)


def size_text(size):
    """A size an operator reads at a glance."""
    if size >= 1_000_000:
        return "%.1f MB" % (size / 1_000_000)
    if size >= 1_000:
        return "%d KB" % (size // 1_000)
    return "%d B" % size


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

    def delete(self, file_hash):
        """Forget a file and what is kept beside it."""
        with self._lock:
            for suffix in ("", ".json", ".part", ".json.part"):
                try:
                    os.unlink(self._path(file_hash) + suffix)
                except (OSError, ValueError):
                    pass

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


# ---- the offer: what crosses the mesh in place of ATAK's notice ------------
#
# ATAK's b-f-t-r notice is about 390 bytes compressed -- two fragments before a
# thumbnail. The offer carries what a receiver needs to rebuild that notice,
# plus, for a QuickPic, where the picture is and a thumbnail of it, in at most
# three fragments: the drawing's cost, proven on the radios.
#
#     FILE_OFFER_V1   kind(1) sender_id(4) sha256(32) size(4) flags(1)
#                     name_len(1) filename(utf-8)
#       FLAG_POINT    lat_e7(4) lon_e7(4) marker_uuid(16)
#       FLAG_THUMB    thumb_len(2) thumbnail(webp)

FLAG_POINT = 0x01
FLAG_THUMB = 0x02
MAX_OFFER_NAME_BYTES = 60

# Three fragments: 747 bytes. See cot_fragment.MAX_FRAGMENT_BYTES.
MAX_OFFER_BYTES = 3 * 249

_OFFER_HEAD = struct.Struct(">BI32sIBB")
_POINT = struct.Struct(">ii16s")


def encode_offer(sender_id, file_hash, size, filename, point=None, thumbnail=None):
    """An offer frame. `point` is (lat_e7, lon_e7, marker_uuid_bytes)."""
    name = (filename or "").encode("utf-8")[:MAX_OFFER_NAME_BYTES]
    flags = (FLAG_POINT if point else 0) | (FLAG_THUMB if thumbnail else 0)
    out = _OFFER_HEAD.pack(tak_payload.FILE_OFFER_V1, sender_id & 0xFFFFFFFF,
                           bytes.fromhex(file_hash), size & 0xFFFFFFFF, flags, len(name)) + name
    if point:
        out += _POINT.pack(point[0], point[1], point[2])
    if thumbnail:
        out += struct.pack(">H", len(thumbnail)) + bytes(thumbnail)
    return out


def decode_offer(frame):
    """A dict, or None for anything that is not a well-formed offer."""
    if (not isinstance(frame, (bytes, bytearray)) or len(frame) < _OFFER_HEAD.size
            or frame[0] != tak_payload.FILE_OFFER_V1):
        return None
    frame = bytes(frame)
    _, sender_id, raw_hash, size, flags, name_len = _OFFER_HEAD.unpack(frame[:_OFFER_HEAD.size])
    at = _OFFER_HEAD.size + name_len
    if at > len(frame):
        return None
    offer = {"sender_id": sender_id, "hash": raw_hash.hex(), "size": size,
             "filename": frame[_OFFER_HEAD.size:at].decode("utf-8", "replace"),
             "point": None, "thumbnail": None}
    if flags & FLAG_POINT:
        if at + _POINT.size > len(frame):
            return None
        offer["point"] = _POINT.unpack(frame[at:at + _POINT.size])
        at += _POINT.size
    if flags & FLAG_THUMB:
        if at + 2 > len(frame):
            return None
        length = struct.unpack(">H", frame[at:at + 2])[0]
        at += 2
        if at + length != len(frame):
            return None
        offer["thumbnail"] = frame[at:at + length]
    elif at != len(frame):
        return None
    return offer


def _package_parts(data):
    """(image entry, image bytes, marker xml) from a data package, or Nones."""
    import io
    import zipfile
    try:
        package = zipfile.ZipFile(io.BytesIO(data))
    except (zipfile.BadZipFile, ValueError):
        return None, None, None
    image = marker = None
    for entry in package.namelist():
        lower = entry.lower()
        if image is None and lower.endswith((".jpg", ".jpeg", ".png", ".webp")):
            image = entry
        elif marker is None and lower.endswith(".cot"):
            text = package.read(entry).decode("utf-8", "replace")
            if "b-i-x-i" in text:
                marker = text
    return image, (package.read(image) if image else None), marker


def quickpic_point(marker_xml):
    """(lat_e7, lon_e7, uuid bytes) of a QuickPic marker, or None."""
    import uuid
    m_uid = re.search(r"""\buid=['"]([0-9a-fA-F-]{36})['"]""", marker_xml or "")
    m_pt = re.search(r"""<point[^>]*\blat=['"]([-0-9.]+)['"][^>]*\blon=['"]([-0-9.]+)['"]""",
                     marker_xml or "")
    if not m_uid or not m_pt:
        return None
    try:
        return (int(round(float(m_pt.group(1)) * 1e7)), int(round(float(m_pt.group(2)) * 1e7)),
                uuid.UUID(m_uid.group(1)).bytes)
    except ValueError:
        return None


# Tried in order, best first; the first that fits the budget wins. Measured on
# a real 4032x3024 QuickPic: 96x72 at quality 15 is 614 B, 80x60 at 25 is 620 B.
THUMB_STEPS = ((112, 20), (96, 20), (96, 12), (80, 20), (64, 20), (64, 10), (48, 10))


def make_thumbnail(image_bytes, budget):
    """A WebP thumbnail of at most `budget` bytes, or None if none fits."""
    try:
        import io
        from PIL import Image
        source = Image.open(io.BytesIO(image_bytes))
        source.draft("RGB", (256, 256))
        source = source.convert("RGB")
    except Exception:          # no Pillow, or not an image: no thumbnail
        return None
    for side, quality in THUMB_STEPS:
        small = source.copy()
        small.thumbnail((side, side))
        out = io.BytesIO()
        small.save(out, "WEBP", quality=quality, method=6)
        if len(out.getvalue()) <= budget:
            return out.getvalue()
    return None


def offer_for(sender_id, file_hash, filename, data):
    """The offer for a stored file: with a point and thumbnail if it is a
    QuickPic, and never over MAX_OFFER_BYTES."""
    _, image, marker = _package_parts(data)
    point = quickpic_point(marker)
    bare = encode_offer(sender_id, file_hash, len(data), filename, point)
    thumbnail = None
    if image is not None:
        thumbnail = make_thumbnail(image, MAX_OFFER_BYTES - len(bare) - 2)
    return encode_offer(sender_id, file_hash, len(data), filename, point, thumbnail)


def notice_from_offer(offer, sender_uid, sender_callsign, now=None):
    """ATAK's b-f-t-r, rebuilt on the receiving side. The URL is filled in by
    rewrite_notice when the file is here."""
    import uuid
    now = now or datetime.now(timezone.utc)
    stamp = now.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (now.microsecond // 1000)
    lat, lon = (offer["point"][0] / 1e7, offer["point"][1] / 1e7) if offer["point"] else (0.0, 0.0)
    name = offer["filename"][:-4] if offer["filename"].lower().endswith(".zip") else offer["filename"]
    esc = lambda text: (text.replace("&", "&amp;").replace('"', "&quot;")
                        .replace("<", "&lt;").replace(">", "&gt;"))
    return ('<event version="2.0" uid="%s" type="%s" how="h-e" time="%s" start="%s" stale="%s">'
            '<point lat="%.7f" lon="%.7f" hae="9999999.0" ce="9999999.0" le="9999999.0"/><detail>'
            '<fileshare filename="%s" senderUrl="" sizeInBytes="%d" sha256="%s" senderUid="%s" '
            'senderCallsign="%s" name="%s"/></detail></event>'
            % (uuid.uuid5(uuid.NAMESPACE_URL, offer["hash"]), FILESHARE_TYPE, stamp, stamp, stamp,
               lat, lon, esc(offer["filename"]), offer["size"], offer["hash"], esc(sender_uid),
               esc(sender_callsign), esc(name)))


def preview_package(offer, sender_callsign):
    """A data package ATAK imports as the QuickPic's marker with the thumbnail
    attached, under the marker's own uid so the full package replaces it."""
    import io
    import uuid
    import zipfile
    marker_uid = str(uuid.UUID(bytes=offer["point"][2]))
    name = offer["filename"][:-4] if offer["filename"].lower().endswith(".zip") else offer["filename"]
    image_entry = "%s/%s_preview.webp" % (offer["hash"][:32], name.rsplit(".", 1)[0])
    stamp = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.000Z")
    marker = ("<?xml version='1.0' encoding='UTF-8' standalone='yes'?>"
              "<event version='2.0' uid='%s' type='b-i-x-i' time='%s' start='%s' "
              "stale='2099-01-01T00:00:00.000Z' how='h-g-i-g-o'>"
              "<point lat='%.7f' lon='%.7f' hae='9999999.0' ce='9999999.0' le='9999999.0'/>"
              "<detail><contact callsign='%s preview'/><remarks>Preview over LoRa; the full "
              "picture follows on a fast path.</remarks></detail></event>"
              % (marker_uid, stamp, stamp, offer["point"][0] / 1e7, offer["point"][1] / 1e7,
                 sender_callsign.replace("'", "")))
    manifest = ('<?xml version="1.0" encoding="UTF-8"?><MissionPackageManifest version="2">'
                '<Configuration><Parameter name="uid" value="%s"/>'
                '<Parameter name="name" value="%s (preview)"/>'
                '<Parameter name="onReceiveImport" value="true"/>'
                '<Parameter name="onReceiveDelete" value="true"/></Configuration><Contents>'
                '<Content ignore="false" zipEntry="%s/%s.cot"><Parameter name="uid" value="%s"/></Content>'
                '<Content ignore="false" zipEntry="%s"><Parameter name="uid" value="%s"/></Content>'
                '</Contents></MissionPackageManifest>'
                % (marker_uid, name, marker_uid, marker_uid, marker_uid, image_entry, marker_uid))
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as package:
        package.writestr("MANIFEST/manifest.xml", manifest)
        package.writestr("%s/%s.cot" % (marker_uid, marker_uid), marker)
        package.writestr(image_entry, offer["thumbnail"])
    return out.getvalue(), "%s_preview.zip" % name.rsplit(".", 1)[0]
