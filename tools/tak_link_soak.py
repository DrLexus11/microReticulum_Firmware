#!/usr/bin/env python3
"""Measure how reliably a Link establishes, and how long it takes.

One link that comes up proves a link can come up. It says nothing about
whether chat works, because LXMF retries five times ten seconds apart -- so a
path where establishment succeeds half the time delivers a message in thirty
seconds rather than three, and a path where it succeeds rarely delivers in five
minutes or not at all. Both look identical to a single dial.

Measured on hardware 2026-09-13: a link came up in 1.6 s while chat took 36.3 s
and the handset's store syncs were hitting a 300 s watchdog. Nothing was
broken. Establishment was marginal, and marginal is invisible to one sample.

    tools/tak_link_soak.py --to urtn-<peer uid> --attempts 20

Everything that matters in PR C rides a Link -- proof-backed delivery, retry
and store-and-forward. Positions and markers do not, which is exactly why they
stayed reliable while chat did not, and why this number is the one to improve.

Report it with the path: a success rate is meaningless without knowing which
hops it crossed.
"""

import argparse
import statistics
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import tak_identity
import tak_lxmf


def dial(RNS, inbox, seconds):
    """One attempt. Returns (established, elapsed, rtt)."""
    started = time.time()
    link = RNS.Link(inbox)
    deadline = started + seconds
    while time.time() < deadline and link.status not in (RNS.Link.ACTIVE,
                                                         RNS.Link.CLOSED):
        time.sleep(0.1)
    elapsed = time.time() - started
    established = link.status == RNS.Link.ACTIVE
    rtt = link.rtt if established else None
    try:
        link.teardown()
    except Exception:
        pass
    return established, elapsed, rtt


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--to", required=True)
    parser.add_argument("--config", default=None)
    parser.add_argument("--attempts", type=int, default=20)
    parser.add_argument("--link-seconds", type=int, default=45,
                        help="how long to wait for one attempt (default 45)")
    parser.add_argument("--settle-seconds", type=float, default=5.0,
                        help="pause between attempts, so a failed link's "
                             "teardown does not contend with the next dial "
                             "(default 5)")
    args = parser.parse_args()

    import RNS
    RNS.Reticulum(args.config)

    node = tak_identity.destination_for(args.to)
    if node is None:
        sys.exit("not a UID this tool can reverse: %r" % args.to)
    identity = RNS.Identity.recall(node)
    if identity is None:
        RNS.Transport.request_path(node)
        sys.exit("no identity for %s yet; asked for a path" % args.to)

    inbox = tak_lxmf.delivery_destination(identity, RNS.Destination.OUT)
    if not RNS.Transport.has_path(inbox.hash):
        RNS.Transport.request_path(inbox.hash)
        deadline = time.time() + 30
        while time.time() < deadline and not RNS.Transport.has_path(inbox.hash):
            time.sleep(0.5)
    if not RNS.Transport.has_path(inbox.hash):
        sys.exit("no path to %s -- nothing to measure" % inbox.hash.hex())

    hops = RNS.Transport.hops_to(inbox.hash)
    print("inbox %s, %d hop(s), %d attempts"
          % (inbox.hash.hex(), hops, args.attempts), flush=True)

    times, rtts, failures = [], [], 0
    for index in range(args.attempts):
        established, elapsed, rtt = dial(RNS, inbox, args.link_seconds)
        if established:
            times.append(elapsed)
            if rtt:
                rtts.append(rtt)
            print("  %2d/%d  up   %5.1fs  rtt %.2fs"
                  % (index + 1, args.attempts, elapsed, rtt or -1), flush=True)
        else:
            failures += 1
            print("  %2d/%d  FAIL %5.1fs" % (index + 1, args.attempts, elapsed),
                  flush=True)
        if index + 1 < args.attempts:
            time.sleep(args.settle_seconds)

    ok = len(times)
    print()
    print("established %d/%d  (%.0f%%)" % (ok, args.attempts,
                                           100.0 * ok / args.attempts))
    if times:
        print("establish   min %.1fs  median %.1fs  max %.1fs"
              % (min(times), statistics.median(times), max(times)))
    if rtts:
        print("rtt         min %.2fs  median %.2fs  max %.2fs"
              % (min(rtts), statistics.median(rtts), max(rtts)))
    if failures:
        # What a success rate costs, in the only terms that matter to an
        # operator. LXMF retries every ten seconds, five times.
        rate = ok / args.attempts if args.attempts else 0
        if rate > 0:
            expected_tries = 1.0 / rate
            print("expect roughly %.1f attempts per delivery, so about %.0fs "
                  "before a message lands at LXMF's ten-second retry"
                  % (expected_tries, (expected_tries - 1) * 10))
        else:
            print("nothing established; chat, retry and store-and-forward all "
                  "fail on this path while packets keep crossing it")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
