#!/usr/bin/env python3
"""Time a Link to a peer's LXMF inbox. Seconds to run, and it ends an argument.

The one diagnostic that separates "packets cross but links do not" from every
other kind of trouble. On 2026-09-13 that distinction was the difference
between three unrelated-looking faults and one, and it took most of a day to
see because nothing measured it directly:

    positions   packet   reliable
    markers     packet   extremely reliable
    chat        Link     never arrived

Chat, proof-backed delivery, retry and store-and-forward all ride a Link.
Positions and markers do not. So when chat is the only thing failing, this is
the first thing to run, not the last.

    tools/tak_link_check.py --to urtn-<peer uid>

It resolves a path first and says so separately, because "no path" and "path
but no link" are different faults with different answers -- and a link dialled
without a path fails in a way that looks like the second while being the first.
"""

import argparse
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tak_identity
import tak_lxmf


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", required=True,
                        help="the peer's urtn- UID, as the bridge prints it")
    parser.add_argument("--config", default=None,
                        help="Reticulum config directory")
    parser.add_argument("--path-seconds", type=int, default=30)
    parser.add_argument("--link-seconds", type=int, default=60)
    args = parser.parse_args()

    import RNS
    RNS.Reticulum(args.config)

    node = tak_identity.destination_for(args.to)
    if node is None:
        sys.exit("not a UID this tool can reverse: %r" % args.to)

    identity = RNS.Identity.recall(node)
    if identity is None:
        RNS.Transport.request_path(node)
        sys.exit("no identity for %s yet -- asked for a path, try again in a "
                 "moment. Until a peer has announced, there is nothing to dial."
                 % args.to)

    inbox = tak_lxmf.delivery_destination(identity, RNS.Destination.OUT)
    print("inbox %s" % inbox.hash.hex())

    if not RNS.Transport.has_path(inbox.hash):
        RNS.Transport.request_path(inbox.hash)
        deadline = time.time() + args.path_seconds
        while time.time() < deadline and not RNS.Transport.has_path(inbox.hash):
            time.sleep(0.5)
    if not RNS.Transport.has_path(inbox.hash):
        print("NO PATH after %ds. Nothing is wrong with links; there is no "
              "route to this destination at all." % args.path_seconds)
        return 2
    print("path known, %d hop(s)" % RNS.Transport.hops_to(inbox.hash))

    started = time.time()
    link = RNS.Link(inbox)
    deadline = started + args.link_seconds
    while time.time() < deadline and link.status not in (RNS.Link.ACTIVE,
                                                         RNS.Link.CLOSED):
        time.sleep(0.2)
    took = time.time() - started
    names = {RNS.Link.ACTIVE: "ACTIVE", RNS.Link.CLOSED: "CLOSED",
             RNS.Link.PENDING: "PENDING", RNS.Link.HANDSHAKE: "HANDSHAKE"}
    state = names.get(link.status, str(link.status))

    if link.status == RNS.Link.ACTIVE:
        print("link ACTIVE after %.1fs, RTT %.2fs" % (took, link.rtt or -1))
        result = 0
    else:
        print("link %s after %.1fs -- a path exists and packets cross it, so "
              "chat, retry and store-and-forward will all fail while markers "
              "and positions keep working." % (state, took))
        result = 1
    try:
        link.teardown()
    except Exception:
        pass
    return result


if __name__ == "__main__":
    sys.exit(main())
