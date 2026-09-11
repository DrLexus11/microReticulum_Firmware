"""Teams as Reticulum GROUP destinations, so broadcast needs no central relay.

TAK already distinguishes sending to a contact from broadcasting to a team, and
Reticulum already has both: a SINGLE destination and a GROUP destination with a
shared key. Mapping one onto the other means a team keeps working when the
command post is out of range, because command was never in the path -- it is a
member of the group, not a hop in it. See docs/TAKNative.md, decision 3.

Joining a team is therefore a key, not a registration. The key is derived from
the team name and a fleet secret, so two nodes provisioned with the same secret
agree on the destination without being told about each other.

The fleet secret is the whole of the security here. Anyone holding it can read
and send to every team derived from it, exactly as anyone holding an IFAC
passphrase can join the interface. It is read from the environment or a file
and never accepted on a command line, so it stays out of shell history and
process listings.
"""

import hashlib
import hmac
import os
import stat
from pathlib import Path

APP = "rnstransport"
ASPECTS = ("tak", "group")
# Domain separation: this secret is also the IFAC passphrase's neighbour in an
# operator's head, and a derivation that did not say what it was for could be
# reused somewhere it should not be.
DOMAIN = b"urtn-tak-group-v1\0"
SECRET_ENVIRONMENT = "TAK_FLEET_SECRET"
DEFAULT_SECRET_PATH = "~/.impr-tak/fleet-secret"
MIN_SECRET_BYTES = 16

# Spelled out rather than left to bytes.strip()'s default so the Kotlin side can
# match it exactly. Kotlin's String.trim() is Unicode-aware and would strip more
# than this, which for a shared secret means deriving a different key from the
# same characters.
ASCII_WHITESPACE = b" \t\n\r\x0b\x0c"
# 64, not 32. RNS's Token picks its cipher from the key length: 32 bytes
# selects AES-128-CBC with a 16-byte signing key, 64 selects AES-256-CBC with a
# 32-byte one. A 32-byte key is accepted without complaint, so the weaker
# choice would never have announced itself. Deriving 64 costs nothing here.
GROUP_KEY_BYTES = 64


def normalise_team(name):
    """Case and surrounding space must not fork a team in half.

    'cyan' and 'Cyan ' are the same team to an operator, and two members whose
    keys disagree would each believe they were broadcasting while hearing
    nothing -- the least debuggable failure this design can have.
    """
    if not isinstance(name, str):
        raise ValueError("team name must be a string")
    collapsed = " ".join(name.split()).casefold()
    if not collapsed:
        raise ValueError("team name must not be empty")
    return collapsed


def load_fleet_secret(path=None, environment=None):
    """The fleet secret, from the environment or a private file.

    Never a command-line argument: `ps` is readable by every user on the box and
    shell history outlives the exercise.
    """
    environment = os.environ if environment is None else environment
    supplied = environment.get(SECRET_ENVIRONMENT)
    if supplied:
        secret = supplied.encode("utf-8")
    else:
        secret = None
    if secret is None:
        secret_path = Path(path or DEFAULT_SECRET_PATH).expanduser()
        if not secret_path.exists():
            raise ValueError(
                "No fleet secret. Set %s, or write one to %s with mode 600."
                % (SECRET_ENVIRONMENT, secret_path))
        info = secret_path.lstat()
        if stat.S_ISLNK(info.st_mode) or not stat.S_ISREG(info.st_mode):
            raise ValueError("Fleet secret must be a regular file: %s" % secret_path)
        if info.st_mode & (stat.S_IRWXG | stat.S_IRWXO):
            raise ValueError(
                "Fleet secret is group- or world-accessible: %s" % secret_path)
        secret = secret_path.read_bytes()
    # Stripped whichever way it arrived. A file written with `echo` carries a
    # trailing newline and stripping it is why this is here at all -- but
    # stripping only that route meant the same provisioning material derived
    # one key from a file and a different one from the environment, or from an
    # operator pasting it into a phone with a stray space. Two members whose
    # keys disagree each broadcast happily and hear nothing.
    secret = secret.strip(ASCII_WHITESPACE)
    if len(secret) < MIN_SECRET_BYTES:
        raise ValueError("Fleet secret must be at least %d bytes" % MIN_SECRET_BYTES)
    return secret


def group_key(team, secret):
    """The shared symmetric key for a team.

    HMAC rather than a bare hash so that the secret is a key and not a prefix,
    which keeps length-extension out of the conversation entirely.
    """
    return hmac.new(secret, DOMAIN + normalise_team(team).encode("utf-8"),
                    hashlib.sha512).digest()[:GROUP_KEY_BYTES]


def group_aspects(team, secret):
    """Aspects naming this team, without naming it in the clear.

    RNS derives a GROUP destination's hash from `app_name` and aspects alone --
    the identity is None for GROUP -- so whatever goes in the aspects is what
    the address is made of, and the address is visible to anyone who can hear an
    announce. A literal "cyan" would let a passive listener enumerate which
    teams exist and count their traffic. The label is an HMAC of the team name
    under the fleet secret instead, so it is stable for everyone holding the
    secret and opaque to everyone else.
    """
    label = hmac.new(secret, DOMAIN + b"aspect\0" + normalise_team(team).encode("utf-8"),
                     hashlib.sha256).hexdigest()[:32]
    return ASPECTS + (label,)


def group_identity_key(team, secret):
    """The 64-byte private key every member of this team derives alike."""
    return hmac.new(secret, DOMAIN + b"identity\0" + normalise_team(team).encode("utf-8"),
                    hashlib.sha512).digest()


def group_identity(team, secret):
    """A shared Reticulum identity for the team.

    A GROUP destination cannot be built with `identity=None`, and finding out
    why is worth recording. RNS accepts it for an IN destination -- and quietly
    *generates a random identity*, appending its hex hash to the aspects. Two
    members would each get a different address, both believe they were joined,
    and neither ever hear the other. It refuses it outright for OUT.

    So the identity is derived from the same secret as the key, which makes it
    the same identity on every member's node.
    """
    import RNS
    return RNS.Identity.from_bytes(group_identity_key(team, secret))


def group_destination(team, secret, direction):
    """The team's destination, ready to send or receive on."""
    import RNS
    destination = RNS.Destination(group_identity(team, secret), direction,
                                  RNS.Destination.GROUP, APP,
                                  *group_aspects(team, secret))
    destination.load_private_key(group_key(team, secret))
    return destination


def group_destination_hash(team, secret):
    """The destination hash for this team, as RNS derives it.

    Deliberately delegated rather than reimplemented. RNS hashes the expanded
    name to NAME_HASH_LENGTH, then hashes that again to TRUNCATED_HASHLENGTH,
    and a local copy of that would be one upstream change away from computing
    addresses nobody else on the mesh agrees with -- which presents as a team
    that can transmit and never be heard.
    """
    import RNS
    return RNS.Destination.hash(group_identity(team, secret), APP,
                                *group_aspects(team, secret))
