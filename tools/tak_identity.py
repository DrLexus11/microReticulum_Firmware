"""EUD identity derived from Reticulum identity, not invented alongside it.

`cot_gateway.py` built an ATAK UID as `"urtn-%08x" % sender_id`, where
`sender_id` is a 32-bit truncation of a 128-bit Reticulum identity hash. That
UID could not be verified, could not be reversed to address a peer, did not
survive a re-provision, and carried no team. Every track, marker and mission
membership downstream inherited those limits. See docs/TAKIntegrationPivots.md.

The UID is derived from the full destination hash here. Length costs nothing on
the air: the mesh carries compact binary and CoT is rendered locally at each
endpoint, so the UID only ever exists inside a 127.0.0.1 socket.

Callsign and team are *claims* until an announce carries them, which is what
`app_data` is for. Nothing in this module treats an unsigned claim as evidence;
`announce_payload` and `parse_announce` move them, and the caller decides what
a claim from an unverified announce is worth.
"""

import struct

# A UID has to survive being pasted into ATAK, a filename and a URL, so it is
# hex rather than base64: no padding, no case folding, no '/' or '+'.
UID_PREFIX = "urtn-"
DESTINATION_HASH_LENGTH = 16

ANNOUNCE_VERSION = 1
MAX_CALLSIGN = 44          # ATAK truncates well before this; it is a bound, not a target.
MAX_TEAM = 24
# ATAK's own team colours. Anything else is carried through untouched -- this
# list is for validation of our own settings, not a restriction on what a peer
# may call itself.
KNOWN_TEAMS = ("White", "Yellow", "Orange", "Magenta", "Red", "Maroon", "Purple",
               "Dark Blue", "Blue", "Cyan", "Teal", "Green", "Dark Green", "Brown")
DEFAULT_TEAM = "Cyan"
DEFAULT_ROLE = "Team Member"

# This node's own destination, which is what a UID must name.
#
# Not the team's group destination: every member of a team derives that same
# address, so a UID built from it is the same string on every node -- one track
# on the map for the whole team, jumping between everyone's positions. A UID
# has to name the node, and it has to be reversible to an address a peer can
# actually send to, which is the whole of pivot 1.
NODE_APP = "rnstransport"
NODE_ASPECTS = ("tak", "node")

_HEADER = struct.Struct(">BBB")


def uid_for(destination_hash):
    """The stable ATAK UID for a Reticulum destination."""
    if len(destination_hash) != DESTINATION_HASH_LENGTH:
        raise ValueError("destination hash must be %d bytes" % DESTINATION_HASH_LENGTH)
    return UID_PREFIX + destination_hash.hex()


def destination_for(uid):
    """The destination hash a UID was derived from, or None if it is foreign.

    ATAK networks carry UIDs this project never issued -- ANDROID-xxxx from a
    phone, a random GUID for a dropped marker. Those are not addressable here
    and saying so is the point: a caller must not be able to mistake somebody
    else's UID for a peer it can send to.
    """
    if not isinstance(uid, str) or not uid.startswith(UID_PREFIX):
        return None
    body = uid[len(UID_PREFIX):]
    if len(body) != DESTINATION_HASH_LENGTH * 2:
        return None
    # bytes.fromhex() ignores ASCII whitespace, so "0" * 30 + "  " is 32
    # characters long, converts happily, and yields fifteen bytes -- an
    # address this function claims to have rejected. The length check above
    # cannot see that, because it counts characters and fromhex does not.
    if any(character not in "0123456789abcdef" for character in body):
        return None
    try:
        recovered = bytes.fromhex(body)
    except ValueError:
        return None
    # Belt and braces: the character check above already forces this, and a
    # destination hash of the wrong length is the one thing a caller must
    # never receive from here.
    if len(recovered) != DESTINATION_HASH_LENGTH:
        return None
    return recovered


def announce_payload(callsign, team=DEFAULT_TEAM, role=DEFAULT_ROLE):
    """Encode callsign and team for a Reticulum announce's app_data."""
    fields = []
    for value, limit, label in ((callsign, MAX_CALLSIGN, "callsign"),
                                (team, MAX_TEAM, "team"),
                                (role, MAX_TEAM, "role")):
        encoded = value.encode("utf-8", errors="strict")
        if not encoded or len(encoded) > limit:
            raise ValueError("%s must be 1..%d UTF-8 bytes" % (label, limit))
        if any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError("%s must not contain control characters" % label)
        fields.append(encoded)
    return _HEADER.pack(ANNOUNCE_VERSION, len(fields[0]), len(fields[1])) + b"".join(fields)


def parse_announce(payload):
    """Decode announce app_data. Returns None for anything not ours.

    Announces arrive from every node on the mesh, including ones running other
    software entirely, so a payload that is not ours is an ordinary event and
    not an error.
    """
    if not payload or len(payload) < _HEADER.size + 3:
        return None
    version, callsign_length, team_length = _HEADER.unpack_from(payload)
    if version != ANNOUNCE_VERSION:
        return None
    body = payload[_HEADER.size:]
    role_length = len(body) - callsign_length - team_length
    if callsign_length < 1 or team_length < 1 or role_length < 1:
        return None
    if callsign_length > MAX_CALLSIGN or team_length > MAX_TEAM or role_length > MAX_TEAM:
        return None
    try:
        callsign = body[:callsign_length].decode("utf-8", errors="strict")
        team = body[callsign_length:callsign_length + team_length].decode("utf-8", errors="strict")
        role = body[callsign_length + team_length:].decode("utf-8", errors="strict")
    except ValueError:
        return None
    return {"callsign": callsign, "team": team, "role": role}


def node_destination(identity, direction):
    """This node's addressable TAK destination.

    SINGLE, so it routes: unlike the team's GROUP destination it goes through
    Reticulum's path table and survives more than one hop, which is what makes
    a UID derived from it usable for addressing a peer rather than merely
    naming one.
    """
    import RNS
    return RNS.Destination(identity, direction, RNS.Destination.SINGLE,
                           NODE_APP, *NODE_ASPECTS)


def node_destination_hash(identity):
    """The hash of this node's TAK destination, without registering anything.

    Delegates to RNS rather than recomputing: a second implementation of the
    hash derivation is a second thing that can drift, and when it drifts it
    produces a perfectly plausible address nobody else is on.
    """
    import RNS
    return RNS.Destination.hash(identity, NODE_APP, *NODE_ASPECTS)
