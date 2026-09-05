#!/usr/bin/env python3
"""Originate signed time assertions onto a Reticulum mesh.

Step 5 of docs/TimePropagation.md. This is the credible source: a machine with
NTP or GNSS that signs what its clock says and announces it. Nodes that have
been provisioned with this authority's identity hash adopt from it without
anyone opening a link, running a CLI at each node, or trusting a single relay
in between -- a captured relay can drop or delay an assertion but cannot forge
one, because the signature is checked against a key that was provisioned out of
band.

The carrier is an ordinary Reticulum announce whose app data is the assertion,
so distribution, hop counting and deduplication are the mesh's existing
machinery rather than a protocol of our own.

    python tools/time_authority.py --interval 1800

It prints its identity hash on startup. Provision that onto each node as a time
authority (Provisioning namespace 100, field 9) and nothing else is required.

Freshness, honestly: a node that already has a clock rejects anything older
than what it holds, so a replayed assertion cannot move it. A node with no
clock at all cannot tell a recording from the real thing -- that is what the
nonce challenge in TimeSync.h is for, and it needs a link. A listen-only node
bootstraps from this to "probably right", and says so: the provenance it
records is `signed-beacon`, never `ntp`.
"""

import argparse
import os
import struct
import sys
import time

import RNS
from RNS.vendor import umsgpack

APP_NAME = "rnstransport"
# Python refuses a dot inside a single aspect, microReticulum does not.
# Both expand to the same name -- "rnstransport.time.assertion" -- and it is
# the expanded name that gets hashed, so the two agree on the destination.
ASPECTS = ("time", "assertion")

# Must match time_beacon_signed_bytes() in TimeBeacon.h exactly. Fixed width,
# big-endian, no msgpack: a canonicalisation disagreement between packers would
# produce signatures that can never verify, and that failure is indistinguishable
# from an attack.
DOMAIN = b"urtn-time-beacon-v1"

# RNS::Utilities::OS::WallTimeSource
SOURCE_CODES = {"ntp": 2, "gnss": 4, "rtc": 5, "system": 6}


def signed_bytes(unix_ms, valid_for_s, stratum, source):
    return (DOMAIN + struct.pack(">Q", unix_ms) + struct.pack(">I", valid_for_s)
            + struct.pack(">BB", stratum, source))


def build_assertion(identity, unix_ms, valid_for_s, stratum, source):
    signature = identity.sign(signed_bytes(unix_ms, valid_for_s, stratum, source))
    return umsgpack.packb({
        "v": 1,
        "t": unix_ms,
        "f": valid_for_s,
        "s": stratum,
        "o": source,
        "g": signature,
    })


def load_identity(path):
    if os.path.isfile(path):
        identity = RNS.Identity.from_file(path)
        if identity is not None:
            return identity, False
    identity = RNS.Identity()
    identity.to_file(path)
    return identity, True


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--identity",
                        default=os.path.expanduser("~/.reticulum/time_authority_identity"),
                        help="where the authority's private key lives")
    parser.add_argument("--interval", type=float, default=1800.0,
                        help="seconds between assertions (default 1800)")
    parser.add_argument("--valid-for", type=int, default=7200,
                        help="seconds an assertion may be treated as fresh")
    parser.add_argument("--stratum", type=int, default=1,
                        help="1 for a direct reference; n+1 for time learned at n")
    parser.add_argument("--source", choices=sorted(SOURCE_CODES), default="ntp",
                        help="what this authority's clock is disciplined by")
    parser.add_argument("--once", action="store_true",
                        help="emit a single assertion and exit")
    args = parser.parse_args()

    if not 1 <= args.stratum <= 255:
        parser.error("stratum must be between 1 and 255")

    RNS.Reticulum()
    identity, created = load_identity(args.identity)
    destination = RNS.Destination(identity, RNS.Destination.IN,
                                  RNS.Destination.SINGLE, APP_NAME, *ASPECTS)

    print("time authority %s" % identity.hash.hex())
    print("  identity file : %s%s" % (args.identity, " (created)" if created else ""))
    print("  destination   : %s" % destination.hash.hex())
    print("  stratum %d, source %s, valid for %ds, every %.0fs"
          % (args.stratum, args.source, args.valid_for, args.interval))
    print("  provision this identity hash as a time authority on each node")
    sys.stdout.flush()

    source_code = SOURCE_CODES[args.source]
    emitted = 0
    while True:
        unix_ms = int(time.time() * 1000)
        destination.announce(build_assertion(identity, unix_ms, args.valid_for,
                                             args.stratum, source_code))
        emitted += 1
        print("[%s] asserted %d ms at stratum %d (emission %d)"
              % (time.strftime("%H:%M:%S"), unix_ms, args.stratum, emitted))
        sys.stdout.flush()
        if args.once:
            # An announce is handed to the interfaces asynchronously; leaving
            # immediately can drop it before it is written.
            time.sleep(3)
            return
        time.sleep(args.interval)


if __name__ == "__main__":
    main()
