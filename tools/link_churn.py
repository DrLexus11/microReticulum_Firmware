#!/usr/bin/env python3
"""Open and close links to a node, again and again, to see what each one costs it.

A board kept ~8 KB of internal heap per link that ended with a request still
pending -- a link <-> receipt cycle shared_ptr could not free -- until it ran out
and restarted (CarriedIssues #1, 2026-09-28). Links arrive on their own schedule
(syncs, page fetches), so a leak shows up slowly and unevenly. This drives them
on demand: establish, optionally request a path, tear down, wait, repeat. Watch
the board's [mem] and [diag] lines (tools/serial_soak.py) while it runs; after
each link every count should be back where it was.

    tools/link_churn.py --to a12176153f8ed49e932367af4b44236b \\
        --aspects nomadnetwork.node --request /page/index.mu --count 10 --interval 240

Also --no-teardown, to leave a link to time out on its own, and --abandon, to
tear down while the request is still pending -- the case that leaked.

Uses the shared Reticulum instance, so run it on a host with rnsd up.
"""

import argparse
import sys
import threading
import time

import RNS


def one_link(destination, request_path, timeout, abandon, no_teardown):
    """Establish, request, tear down. Returns a short outcome line."""
    established = threading.Event()
    answered = threading.Event()
    started = time.time()
    link = RNS.Link(destination, established_callback=lambda _l: established.set())
    if not established.wait(timeout):
        link.teardown()
        return "no link within %.0f s" % timeout
    setup = time.time() - started
    outcome = "link up in %.1f s" % setup
    if request_path:
        link.request(request_path, data=None,
                     response_callback=lambda _r: answered.set(),
                     failed_callback=lambda _r: answered.set(), timeout=timeout)
        if abandon:
            outcome += ", abandoned with the request pending"
        elif answered.wait(timeout):
            outcome += ", request answered"
        else:
            outcome += ", request unanswered"
    if not no_teardown:
        link.teardown()
        outcome += ", torn down"
    return outcome


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--to", required=True, help="destination hash, hex")
    parser.add_argument("--aspects", default="nomadnetwork.node",
                        help="app name and aspects, dotted (default nomadnetwork.node)")
    parser.add_argument("--request", help="a request path to ask on each link, e.g. /page/index.mu")
    parser.add_argument("--count", type=int, default=5)
    parser.add_argument("--interval", type=float, default=240.0,
                        help="seconds between links; longer than the board's one-minute report")
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--abandon", action="store_true",
                        help="tear down while the request is still pending")
    parser.add_argument("--no-teardown", action="store_true",
                        help="leave the link to time out on its own")
    args = parser.parse_args()

    RNS.Reticulum(loglevel=RNS.LOG_WARNING)
    destination_hash = bytes.fromhex(args.to)
    if not RNS.Transport.has_path(destination_hash):
        RNS.Transport.request_path(destination_hash)
        deadline = time.time() + args.timeout
        while not RNS.Transport.has_path(destination_hash) and time.time() < deadline:
            time.sleep(0.5)
    identity = RNS.Identity.recall(destination_hash)
    if identity is None:
        print("no path to or identity for %s" % args.to, file=sys.stderr)
        return 1
    app, *aspects = args.aspects.split(".")
    destination = RNS.Destination(identity, RNS.Destination.OUT, RNS.Destination.SINGLE, app, *aspects)
    if destination.hash != destination_hash:
        print("%s is not %s's %s destination" % (args.to, identity, args.aspects), file=sys.stderr)
        return 1

    for n in range(1, args.count + 1):
        print("%s link %d/%d: %s" % (time.strftime("%H:%M:%S"), n, args.count,
                                     one_link(destination, args.request, args.timeout,
                                              args.abandon, args.no_teardown)), flush=True)
        if n < args.count:
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
