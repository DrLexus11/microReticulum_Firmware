#!/usr/bin/env python3
"""Supply UTC to a node over the mesh, on whatever interface reaches it.

There is no NTP here and there does not need to be. NTP is an IP protocol whose
accuracy model assumes a symmetric, low-latency path; over LoRa or ESP-NOW that
model does not hold and the protocol has nothing to offer us. What a node
actually needs is a trusted peer to say what time it is, which Reticulum already
carries: the request rides an authenticated Link over LoRa, ESP-NOW, BLE, UDP or
TCP without knowing or caring which.

The node's /time handler applies its own rules -- never backwards, bounded
forward step, provenance recorded -- and answers with what it decided, so a
refusal is visible rather than silent.

Accuracy is bounded by the round trip. We send the midpoint estimate: the time
we expect it to be when the request lands, which is now plus half the measured
RTT. Over LoRa that leaves errors of tenths of a second; message expiry, stamp
validation and CoT staleness are minute-scale concerns, so that is ample.
"""

import argparse
import os
import sys
import time

import RNS

USAGE = "usage: set_node_time.py <node-hash-hex> [--identity FILE] [--dry-run]"


def main():
    ap = argparse.ArgumentParser(usage=USAGE)
    ap.add_argument("node", help="the node's NomadNet destination hash, as shown "
                                 "on its index page")
    ap.add_argument("--identity", default="~/.nomadnetwork/storage/identity",
                    help="identity to present; must be on the node's "
                         "remote-management allow list")
    ap.add_argument("--dry-run", action="store_true",
                    help="resolve and connect, but do not set the clock")
    ap.add_argument("--timeout", type=float, default=120.0)
    args = ap.parse_args()

    try:
        node_hash = bytes.fromhex(args.node)
    except ValueError:
        sys.exit("not a hex destination hash: %r\n%s" % (args.node, USAGE))
    if len(node_hash) != RNS.Reticulum.TRUNCATED_HASHLENGTH // 8:
        sys.exit("destination hash is %d bytes, expected %d" % (
            len(node_hash), RNS.Reticulum.TRUNCATED_HASHLENGTH // 8))

    RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"),
                  loglevel=int(os.environ.get("RNSLL", "2")))
    identity = RNS.Identity.from_file(os.path.expanduser(args.identity))
    print("our identity:", RNS.hexrep(identity.hash, delimit=False))

    if not RNS.Transport.has_path(node_hash):
        RNS.Transport.request_path(node_hash)
        t0 = time.time()
        while not RNS.Transport.has_path(node_hash) and time.time() - t0 < 45:
            time.sleep(0.1)
    if not RNS.Transport.has_path(node_hash):
        sys.exit("no path to %s -- it has not announced, or nothing here can "
                 "reach it" % args.node)

    node_identity = RNS.Identity.recall(node_hash)
    if node_identity is None:
        sys.exit("a path exists but the node's identity is not known yet -- "
                 "wait for an announce")

    # Remote management lives on the same identity, under Transport's own
    # aspects, so it can be derived rather than looked up separately.
    # The firmware passes "remote.management" as a single aspect; Python RNS
    # forbids dots inside one, so split it. Both expand to the same full name,
    # rnstransport.remote.management, and therefore the same destination hash.
    mgmt = RNS.Destination(node_identity, RNS.Destination.OUT,
                           RNS.Destination.SINGLE, "rnstransport",
                           "remote", "management")
    print("remote management destination:",
          RNS.hexrep(mgmt.hash, delimit=False),
          "(node is %d hops away)" % RNS.Transport.hops_to(node_hash))

    # Paths are per destination hash, and the management destination announces
    # on its own schedule -- reaching the node's pages says nothing about
    # whether we can reach its management endpoint yet.
    if not RNS.Transport.has_path(mgmt.hash):
        RNS.Transport.request_path(mgmt.hash)
        t0 = time.time()
        while not RNS.Transport.has_path(mgmt.hash) and time.time() - t0 < 45:
            time.sleep(0.1)
    if not RNS.Transport.has_path(mgmt.hash):
        sys.exit("no path to the management destination -- it may not have "
                 "announced yet, or remote management is disabled on that node")

    state = {"done": False, "ok": False}

    def on_response(receipt):
        print("node replied:", receipt.response)
        state["ok"] = True
        state["done"] = True

    def on_failed(receipt):
        # The allow list is enforced before the handler runs, so this is what a
        # node that does not trust us looks like -- not an error message.
        print("REQUEST FAILED -- our identity is most likely not on this "
              "node's remote-management allow list")
        state["done"] = True

    link = RNS.Link(mgmt)

    def established(lk):
        lk.identify(identity)
        # Aim at the moment the request lands, not the moment we sent it.
        supplied_ms = int((time.time() + lk.rtt / 2.0) * 1000)
        print("link established rtt=%.3fs -- supplying %d ms (%s), "
              "half-RTT ahead by %d ms" % (
                  lk.rtt, supplied_ms,
                  time.strftime("%Y-%m-%dT%H:%M:%SZ",
                                time.gmtime(supplied_ms / 1000)),
                  int(lk.rtt * 500)))
        if args.dry_run:
            print("dry run -- not setting the clock")
            state["done"] = True
            return
        lk.request("/time", data=supplied_ms, response_callback=on_response,
                   failed_callback=on_failed)

    link.set_link_established_callback(established)
    link.set_link_closed_callback(
        lambda lk: print("link closed teardown=%s" % lk.teardown_reason))

    deadline = time.time() + args.timeout
    while not state["done"] and time.time() < deadline:
        time.sleep(0.1)
    if not state["done"]:
        print("TIMEOUT")
    link.teardown()
    time.sleep(0.5)
    return 0 if state["ok"] or args.dry_run else 1


if __name__ == "__main__":
    sys.exit(main())
