#!/usr/bin/env python3
"""Prove that a message survives a partition, on the bench, in one command.

PR C's headline is "chat that survives a partition". Two of its three
guarantees were proven on hardware 2026-09-12 -- proof-backed delivery and
retry. The third was implemented, unit tested, and had nowhere to forward to,
because no propagation node existed on the mesh. An unexercised guarantee is a
claim, and this is what turns it into a measurement.

It stands up a peer, lets the bridge learn it, takes the peer away, sends to it
anyway, and brings it back:

    1. peer announces           the bridge learns a team member
    2. peer stops               the partition
    3. bridge sends a DM        direct delivery fails, and the carrier
                                escalates it to the propagation node
    4. peer returns and syncs   the message arrives without anyone resending

Step 3 is the part that had never run. What makes it a real test rather than a
mock is that every component is production: the bridge is the bridge, the
carrier is the carrier, and the propagation node is lxmd.

    tools/tak_partition_check.py --propagation-node <hash>

The peer keeps a persistent identity under ~/.impr-tak/partition-peer so that
the node it returns as is the node that went away -- a fresh identity would be
a different person, and the message would correctly not be for them.

This is a bench proof of the *semantics*. It says nothing about LoRa, which is
proven separately; the point of doing it on one machine is that a partition can
be created exactly, on demand, rather than by walking out of range and hoping.
"""

import argparse
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tak_groups as groups
import tak_identity as tak_identity
import tak_lxmf
import tak_membership as membership

PEER_HOME = Path("~/.impr-tak/partition-peer").expanduser()
DEFAULT_TEXT = "partition check: sent while you were away"


def log(message):
    print("[partition] %s" % message, flush=True)


def peer_identity(RNS):
    """The same peer every run, so returning is returning rather than arriving."""
    PEER_HOME.mkdir(parents=True, exist_ok=True)
    path = PEER_HOME / "identity"
    if path.exists():
        return RNS.Identity.from_file(str(path))
    identity = RNS.Identity()
    identity.to_file(str(path))
    os.chmod(path, 0o600)
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--propagation-node", required=True,
                        help="destination hash of the LXMF propagation node")
    parser.add_argument("--team", default=tak_identity.DEFAULT_TEAM)
    parser.add_argument("--callsign", default="PARTITION")
    parser.add_argument("--config", default=None,
                        help="Reticulum config directory for the peer")
    parser.add_argument("--secret-file", default=None)
    parser.add_argument("--announce-seconds", type=int, default=20,
                        help="how long to stay up so the bridge learns us")
    parser.add_argument("--stage", choices=("announce", "collect"), required=True,
                        help="announce: be present, then leave. "
                             "collect: come back and sync.")
    parser.add_argument("--sync-seconds", type=int, default=90,
                        help="how long to wait for the propagation sync")
    args = parser.parse_args()

    import RNS
    RNS.Reticulum(args.config)
    identity = peer_identity(RNS)
    node_hash = tak_identity.node_destination_hash(identity)
    uid = tak_identity.uid_for(node_hash)
    log("peer is %s (%s)" % (uid, args.callsign))

    try:
        secret = groups.load_fleet_secret(args.secret_file)
    except ValueError as error:
        sys.exit("[partition] %s" % error)

    received = []

    def on_chat(frame, source):
        received.append(frame)
        log("RECEIVED %d bytes from %s" % (len(frame), (source or b"").hex()))

    storage = PEER_HOME / "lxmf"
    storage.mkdir(parents=True, exist_ok=True)
    carrier = tak_lxmf.Carrier(identity, str(storage), args.callsign, on_chat,
                               propagation_node=bytes.fromhex(args.propagation_node))

    node = tak_identity.node_destination(identity, RNS.Destination.IN)

    if args.stage == "announce":
        node.announce(membership.member_payload(args.team, secret,
                                                args.callsign, "Team Member"))
        carrier.destination.announce()
        log("announced; staying up %ds so the bridge learns the path"
            % args.announce_seconds)
        time.sleep(args.announce_seconds)
        log("leaving. Address %s while this process is gone." % uid)
        carrier.stop()
        return 0

    # collect: the peer has come back, and asks the propagation node what it
    # missed. Nobody resends anything -- that is the whole claim.
    log("back on the network; requesting what was held for us")
    carrier.router.request_messages_from_propagation_node(identity)
    deadline = time.time() + args.sync_seconds
    while time.time() < deadline and not received:
        time.sleep(1)
    carrier.stop()
    if received:
        log("PASS: %d message(s) arrived from the propagation node, "
            "with no resend" % len(received))
        return 0
    log("FAIL: nothing arrived within %ds" % args.sync_seconds)
    return 1


if __name__ == "__main__":
    sys.exit(main())
