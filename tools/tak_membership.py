"""Who is on this team, learned from announces rather than configured.

Pivot 5 in docs/TAKIntegrationPivots.md. A `GROUP` destination reaches only
peers on the same interface as the sender -- measured, not a judgement call --
so a team cannot be an address. It becomes a membership set instead: each node
announces its own `SINGLE` destination, which routes, and traffic for the team
is addressed to each member in turn.

The team name is **not** carried in the clear. `tak_groups.group_aspects` goes
to some trouble to keep team names off the air, and an announce saying
`team="Cyan"` would hand a passive listener exactly what that avoided. A node
announces an HMAC of the team under the fleet secret instead: everyone holding
the secret recognises it, nobody else can compute it, and nobody without the
secret can forge one either.

What this does not hide is that some set of nodes share a tag. A listener can
count a team and watch it move without ever learning its name. That is the same
exposure `group_aspects` already accepts, and it is worth being clear that
membership is pseudonymous rather than private.
"""

import hashlib
import hmac
import struct
import time

import tak_groups as groups

# Version 2. Version 1 carried the callsign, the team name in plain text, and
# the role; it had no caller on either side, and the team name was the reason
# to replace it before it acquired one.
VERSION = 2

TEAM_TAG_BYTES = 8
MAX_CALLSIGN = 44
MAX_ROLE = 24
_HEADER = struct.Struct(">B%dsBB" % TEAM_TAG_BYTES)
HEADER_BYTES = _HEADER.size

# How long a member is kept after its last announce.
#
# Deliberately long. A responder who has gone quiet is precisely the one still
# worth addressing, and absence of an announce says nothing about reachability
# -- announces are rare by design. Forgetting a member means silently declining
# to send them anything, which is the failure this whole pivot exists to avoid.
DEFAULT_EXPIRY_SECONDS = 6 * 60 * 60

# The shortest gap between two announces prompted by meeting somebody new.
#
# A node that starts late hears everyone who announces after it and nobody who
# announced before -- so meeting a stranger is the moment to say who you are,
# rather than leaving them to wait out a re-announce interval measured in
# half-hours. One exchange is enough: the greeting is only sent for a member
# that was not already known, so the reply it provokes finds a known member and
# stops there. The floor is what keeps a team all starting at once from turning
# that into a storm.
GREET_MIN_INTERVAL_SECONDS = 20

# What an announce turned out to be. Distinguishing "new" from "we already knew
# them" is protocol policy rather than caller bookkeeping: greeting depends on
# it, and a caller working it out by searching the member list would both
# duplicate the rule and get it subtly wrong around expiry.
MEMBER_NEW = "new"
MEMBER_KNOWN = "known"


def team_tag(team, secret):
    """A stable, unforgeable label for a team that does not name it.

    Same domain and the same minimum as every other derivation from the fleet
    secret, so a secret too weak to build a group key cannot quietly be good
    enough to claim membership with.
    """
    if not isinstance(secret, (bytes, bytearray)) or len(secret) < groups.MIN_SECRET_BYTES:
        raise ValueError("fleet secret must be at least %d bytes" % groups.MIN_SECRET_BYTES)
    return hmac.new(bytes(secret),
                    groups.DOMAIN + b"member\0" + groups.normalise_team(team).encode("utf-8"),
                    hashlib.sha256).digest()[:TEAM_TAG_BYTES]


def member_payload(team, secret, callsign, role="Team Member"):
    """Announce app_data claiming membership of a team.

    Claims, not evidence, in one respect only: the callsign and role are
    whatever the node says. The *tag* is evidence, because computing it
    requires the fleet secret.
    """
    fields = []
    for value, limit in ((callsign, MAX_CALLSIGN), (role, MAX_ROLE)):
        if not isinstance(value, str):
            raise ValueError("callsign and role must be text")
        encoded = value.encode("utf-8")
        if not encoded or len(encoded) > limit:
            raise ValueError("field must be 1..%d UTF-8 bytes" % limit)
        if any(ord(character) < 32 or ord(character) == 127 for character in value):
            raise ValueError("field must not contain control characters")
        fields.append(encoded)
    return (_HEADER.pack(VERSION, team_tag(team, secret), len(fields[0]), len(fields[1]))
            + fields[0] + fields[1])


def parse_member(payload):
    """Decode announce app_data, or None for anything that is not ours.

    Announces arrive from every node on the mesh, including ones running other
    software entirely. A payload that is not ours is ordinary and not an error.
    """
    if not isinstance(payload, (bytes, bytearray)) or len(payload) < HEADER_BYTES:
        return None
    payload = bytes(payload)
    version, tag, callsign_length, role_length = _HEADER.unpack(payload[:HEADER_BYTES])
    if version != VERSION:
        return None
    if callsign_length < 1 or role_length < 1:
        return None
    if callsign_length > MAX_CALLSIGN or role_length > MAX_ROLE:
        return None
    body = payload[HEADER_BYTES:]
    if len(body) != callsign_length + role_length:
        return None
    try:
        callsign = body[:callsign_length].decode("utf-8", errors="strict")
        role = body[callsign_length:].decode("utf-8", errors="strict")
    except UnicodeDecodeError:
        return None
    return {"tag": tag, "callsign": callsign, "role": role}


class MemberRegistry:
    """The members of one team, as heard on the air.

    Addressed traffic costs one transmission per member per hop, so this list
    is what a marker costs. It is built only from announces carrying our team's
    tag -- an announce from another team, or from software that is not ours, is
    not a member and is not paid for.
    """

    def __init__(self, team, secret, expiry_seconds=DEFAULT_EXPIRY_SECONDS, own_hash=None):
        self.tag = team_tag(team, secret)
        self.expiry = expiry_seconds
        self.own_hash = own_hash
        self._members = {}
        self._last_greet = None

    def remember(self, destination_hash, app_data, now=None):
        """Record an announce.

        Returns MEMBER_NEW for a member we had not got, MEMBER_KNOWN for one we
        had, and None for an announce that is not this team's -- which is most
        of them, since this is called for every announce the node hears.

        The new/known distinction is the whole basis of greeting, and keeping
        it here rather than leaving callers to search the member list means one
        rule, applied the same way, including around expiry.
        """
        claims = parse_member(app_data)
        if claims is None:
            return None
        if not hmac.compare_digest(claims["tag"], self.tag):
            return None
        destination_hash = bytes(destination_hash)
        if self.own_hash is not None and destination_hash == bytes(self.own_hash):
            # Our own announce comes back through Transport like anyone else's.
            # Addressing ourselves would double every marker we send and put
            # our own events back into our own endpoint.
            return None
        now = time.time() if now is None else now
        previous = self._members.get(destination_hash)
        # An entry past its expiry is not a member any more, so hearing from it
        # again is a new arrival rather than a refresh -- and worth greeting,
        # because from their side we may equally have dropped off.
        known = previous is not None and now - previous["heard"] <= self.expiry
        self._members[destination_hash] = {
            "callsign": claims["callsign"],
            "role": claims["role"],
            "heard": now,
        }
        return MEMBER_KNOWN if known else MEMBER_NEW

    def should_greet(self, now=None):
        """Whether to announce now because we have just met someone new.

        Rate limited rather than unconditional: ten nodes powering up together
        would otherwise each announce nine times, which is a lot of the most
        expensive packet Reticulum has.
        """
        now = time.time() if now is None else now
        if self._last_greet is not None and now - self._last_greet < GREET_MIN_INTERVAL_SECONDS:
            return False
        self._last_greet = now
        return True

    def members(self, now=None):
        """Destination hashes worth addressing, newest announce first."""
        now = time.time() if now is None else now
        live = [(dest, entry) for dest, entry in self._members.items()
                if now - entry["heard"] <= self.expiry]
        live.sort(key=lambda item: item[1]["heard"], reverse=True)
        return [dest for dest, _ in live]

    def describe(self, destination_hash):
        """Callsign and role as claimed, or None. For display, not for trust."""
        entry = self._members.get(bytes(destination_hash))
        return None if entry is None else dict(entry)

    def forget_expired(self, now=None):
        """Drop members past the expiry. Returns how many went."""
        now = time.time() if now is None else now
        stale = [dest for dest, entry in self._members.items()
                 if now - entry["heard"] > self.expiry]
        for dest in stale:
            del self._members[dest]
        return len(stale)

    def __len__(self):
        return len(self._members)
