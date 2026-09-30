#!/usr/bin/env python3
"""Read a board's NomadNet page over the mesh.

A board on a wall plug has no serial port to read. Its NomadNet node serves
/page/device.mu, whose "boot" category reports uptime, the last reset reason
and the running crash and panic totals -- which is how a soak follows it:

    tools/node_page.py a12176153f8ed49e932367af4b44236b /page/device.mu boot
    tools/node_page.py <hash> /page/device.mu interfaces
    tools/node_page.py <hash> /page/time.mu

The device page answers only a peer that identifies itself on the link; any
identity will do, so a throwaway one is used. Run it on a host with rnsd up.
"""

import argparse
import sys
import threading
import time

import RNS


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("node", help="the node's NomadNet destination hash, hex")
    parser.add_argument("path", help="the page, e.g. /page/device.mu")
    parser.add_argument("category", nargs="?", help="the page's category (var_c), e.g. boot")
    parser.add_argument("--timeout", type=float, default=120.0)
    args = parser.parse_args()

    RNS.Reticulum(loglevel=RNS.LOG_WARNING)
    node = bytes.fromhex(args.node)
    if not RNS.Transport.has_path(node):
        RNS.Transport.request_path(node)
        deadline = time.time() + 30
        while not RNS.Transport.has_path(node) and time.time() < deadline:
            time.sleep(0.5)
    identity = RNS.Identity.recall(node)
    if identity is None:
        print("no path to %s" % args.node, file=sys.stderr)
        return 1
    destination = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
                                  "nomadnetwork", "node")
    up, done, result = threading.Event(), threading.Event(), {}
    link = RNS.Link(destination, established_callback=lambda _l: up.set())
    if not up.wait(60):
        print("no link to %s" % args.node, file=sys.stderr)
        return 1
    link.identify(RNS.Identity())
    time.sleep(2)

    def answered(receipt):
        result["page"] = receipt.response
        done.set()

    link.request(args.path, {"var_c": args.category} if args.category else None,
                 response_callback=answered, failed_callback=lambda _r: done.set(),
                 timeout=args.timeout)
    done.wait(args.timeout + 10)
    link.teardown()
    page = result.get("page")
    if page is None:
        print("no answer from %s" % args.node, file=sys.stderr)
        return 1
    print(page.decode(errors="replace") if isinstance(page, bytes) else page)
    return 0


if __name__ == "__main__":
    sys.exit(main())
