"""Tier 2: a CoT event small enough to carry, without knowing what it means.

TAKNative.md splits traffic three ways. Tier 2 is the must-arrive tier, and its
job is to carry *any* CoT type -- including ones no typed codec exists for --
so that "can we send drawings" is answered yes on the first day rather than
after every schema has been hand-modelled.

The compression is the whole design, and it was chosen by measurement against
400 real ATAK events rather than by reputation:

    raw                                   582 B mean
    deflate, no dictionary                378 B    1.5x
    deflate, curated dictionary           232 B    2.5x
    deflate, dictionary trained on traffic 87 B    6.7x

A single CoT event is a few hundred bytes of XML, which is far too little
history for deflate to work with -- hence the unremarkable 1.5x, and hence
TAKNative.md's earlier claim of "4 to 5 times" being wrong. What CoT has
instead is enormous repetition *between* events, which a preset dictionary
turns into the 2.5x below.

The trained dictionary is not used despite compressing best. It embeds the
callsigns, coordinates and device identifiers of whatever it was trained on,
and this dictionary ships to every node on the mesh.

2.5x is worth more than the ratio suggests: it takes the mean event from 582 to
232 bytes, and a Reticulum packet carries 383. Most CoT stops needing two
packets, which is 189 ms of LoRa airtime instead of 466.
"""

import zlib

VERSION = 1

# Encoding is also the dictionary version. Changing the dictionary changes what
# a payload decompresses to, so it can never be a silent edit: a new dictionary
# takes a new number and old nodes reject it rather than producing plausible
# rubbish.
ENCODING_RAW = 0
ENCODING_DEFLATE_DICT_V1 = 1

# A peer can hand us a small payload that expands enormously. CoT that large is
# already outside tier 2 -- it belongs in tier 3, fetched deliberately -- so the
# bound is a refusal rather than a limitation.
MAX_DECOMPRESSED = 64 * 1024
# One Reticulum packet, which is the whole promise of tier 2. RNS.Packet's
# ENCRYPTED_MDU; hard-coded rather than imported so this module stays usable
# without RNS installed, and asserted against the real value in the tests.
MAX_FRAME_BYTES = 383

_ATTRIBUTES = (
    "version uid type time start stale how access qos opex lat lon hae ce le "
    "callsign endpoint device os platform battery course speed altsrc geopointsrc "
    "name role abbr exrole argb iconsetpath parent_callsign production_time relation "
    "readiness remarks archive precisionlocation usericon creator link status track "
    "contact __group takv detail event point marti dest chat chatgrp senderCallsign"
)
_HOW = "m-g m-g-n m-g-e h-g-i-g-o h-e h-t-l-f m-p"
_ROLES = "Team Member Team Lead HQ Sniper Medic Forward Observer RTO K9"
_TEAMS = ("White Yellow Orange Magenta Red Maroon Purple Dark Blue Blue Cyan Teal "
          "Green Dark Green Brown")
_TYPES = (
    "a-f-G-U-C a-f-G-U-C-I a-f-G-U-C-V a-f-G-E-V a-f-G a-f-A a-f-S a-h-G a-h-A a-h-S "
    "a-n-G a-n-A a-u-G a-u-A b-m-p-s-p-i b-m-p-s-m b-m-p-c-cp b-m-p-w b-m-r b-t-f "
    "b-t-f-r b-t-f-d b-a-o-tbl u-d-f u-d-f-m u-d-r u-d-c-c t-x-c-t t-x-takp-q"
)

# Deflate treats the dictionary as a window of preceding bytes, so the material
# most likely to match goes last.
DICTIONARY = (
    " ".join((_ATTRIBUTES, _HOW, _ROLES, _TEAMS, _TYPES)).encode("utf-8") +
    b'COT_MAPPING_2525C/ 9999999.0 ATAK-CIV Team Member Undefined "/><'
    b'<event version="2.0" uid="" type="" time="" start="" stale="" how=""><point '
    b'lat="" lon="" hae="" ce="9999999.0" le="9999999.0"/><detail>'
    b'<contact callsign=""/><__group name="" role="Team Member"/>'
    b'<precisionlocation altsrc="GPS" geopointsrc="GPS"/><status battery=""/>'
    b'<track course="" speed=""/><takv device="" os="" platform="ATAK-CIV" version=""/>'
    b'<uid Droid=""/><remarks/><archive/><color argb="-1"/>'
    b'<usericon iconsetpath="COT_MAPPING_2525C/"/>'
    b'<link parent_callsign="" production_time="" relation="p-p" type="" uid=""/>'
    b'<creator callsign="" time="" type="" uid=""/></detail></event>'
)


def encode(cot_xml):
    """Frame a CoT event for tier 2, compressed only when that is smaller.

    A very short event can deflate to more than it started as, and paying for
    compression that made the packet bigger is the kind of thing that never
    shows up until somebody measures airtime.
    """
    if isinstance(cot_xml, str):
        cot_xml = cot_xml.encode("utf-8", errors="strict")
    if not isinstance(cot_xml, (bytes, bytearray)):
        raise ValueError("CoT must be text or bytes")
    if not cot_xml:
        raise ValueError("CoT must not be empty")
    if len(cot_xml) > MAX_DECOMPRESSED:
        raise ValueError("CoT is too large for tier 2; it belongs in tier 3")
    compressor = zlib.compressobj(9, zlib.DEFLATED, -15, zdict=DICTIONARY)
    deflated = compressor.compress(bytes(cot_xml)) + compressor.flush()
    if len(deflated) < len(cot_xml):
        frame = bytes([VERSION, ENCODING_DEFLATE_DICT_V1]) + deflated
    else:
        frame = bytes([VERSION, ENCODING_RAW]) + bytes(cot_xml)
    if len(frame) > MAX_FRAME_BYTES:
        # Tier 2 is one packet by definition, and this is the bound that makes
        # it one. Nothing here checked it: a 5 KB ATAK drawing compresses to
        # ~700 bytes, which is comfortably under MAX_DECOMPRESSED and nearly
        # twice the MDU. RNS.Packet then raises OSError, which the bridge's
        # client loop catches as a dead socket -- so drawing a polyline
        # disconnected ATAK rather than reporting anything.
        raise ValueError(
            "tier 2 frame is %d bytes, over the %d-byte bound; it belongs in tier 3"
            % (len(frame), MAX_FRAME_BYTES))
    return frame


def decode(frame):
    """Recover the CoT event, refusing anything that is not plainly ours."""
    if not isinstance(frame, (bytes, bytearray)) or len(frame) < 3:
        raise ValueError("tier 2 frame is too short")
    version, encoding = frame[0], frame[1]
    if version != VERSION:
        raise ValueError("unsupported tier 2 version %d" % version)
    body = bytes(frame[2:])
    if encoding == ENCODING_RAW:
        payload = body
    elif encoding == ENCODING_DEFLATE_DICT_V1:
        decompressor = zlib.decompressobj(-15, zdict=DICTIONARY)
        try:
            payload = decompressor.decompress(body, MAX_DECOMPRESSED)
        except zlib.error as error:
            raise ValueError("tier 2 payload did not decompress: %s" % error) from error
        if decompressor.unconsumed_tail:
            raise ValueError("tier 2 payload expands beyond the tier 2 bound")
        # unconsumed_tail alone does not mean the stream finished. A truncated
        # frame decompresses to whatever prefix it contained, leaves no
        # unconsumed input, and reports eof False -- so a half a CoT event
        # would be handed on as though it were whole.
        if not decompressor.eof:
            raise ValueError("tier 2 payload is truncated")
        if decompressor.unused_data:
            raise ValueError("tier 2 frame has trailing data after the stream")
    else:
        raise ValueError("unknown tier 2 encoding %d" % encoding)
    if not payload:
        raise ValueError("tier 2 payload is empty")
    return payload.decode("utf-8", errors="strict")
