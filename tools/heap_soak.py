#!/usr/bin/env python3
"""Log node heap over hours, to catch degradation that a short test cannot.

Why this exists
---------------
On 2026-09-04 OZD-01 was measured at 0 of 36 link setups. A power cycle -- no
firmware change, no move -- took it to 83%. Its heap told the story:

                     days of uptime      fresh boot
    free                33952 (15%)     55736 (25%)
    largest block             16372           36852

A Reticulum link setup allocates, so a node whose largest contiguous block has
halved stops accepting connections while still announcing perfectly and looking
alive from the outside. In a deployment where nodes sit in apartments for
months, that is a node that bricks itself quietly a fortnight after install --
which is worse than one that fails loudly.

Two endpoints days apart is not a curve. This samples on a cadence long enough
to be cheap and long enough to run for days, so the shape can be seen: a steady
leak, a sawtooth from fragmentation, or a step at some event.

Failures are data too -- a poll that times out is recorded rather than dropped,
because "stopped answering at hour 40" is exactly the result we are looking for.

Usage
-----
  heap_soak.py --target ozd01=ed03dec... --target ozd02=558a7fc0... \\
               --interval 600 --hours 48
  heap_soak.py --plot bench-runs/heap-soak.jsonl
"""

import argparse
import json
import os
import statistics
import sys
import time

import RNS


FIELDS = ("heap_free", "heap_minfree", "heap_maxalloc", "heap_fragmented",
          "heap_size")


def parse_heap(body):
    """The page is JSON-ish with a trailing comma the parser will not take."""
    if not body:
        return None
    text = body.decode("utf-8", "replace") if isinstance(body, bytes) else str(body)
    values = {}
    for line in text.splitlines():
        line = line.strip().rstrip(",")
        if ":" not in line:
            continue
        key, _, raw = line.partition(":")
        key = key.strip().strip('"')
        raw = raw.strip()
        try:
            values[key] = int(raw)
        except ValueError:
            continue
    return values or None


def poll(name, dest_hash, identity, timeout, category="heap"):
    sample = {"target": name, "at": time.time(), "ok": False,
              "category": category}
    if not RNS.Transport.has_path(dest_hash):
        RNS.Transport.request_path(dest_hash)
        deadline = time.time() + min(30.0, timeout)
        while not RNS.Transport.has_path(dest_hash) and time.time() < deadline:
            time.sleep(0.2)
    node_identity = (RNS.Identity.recall(dest_hash)
                     if RNS.Transport.has_path(dest_hash) else None)
    if node_identity is None:
        sample["stage"] = "no-path"
        return sample

    destination = RNS.Destination(node_identity, RNS.Destination.OUT,
                                  RNS.Destination.SINGLE, "nomadnetwork", "node")
    state = {"done": False}
    sample["stage"] = "link"
    link = RNS.Link(destination)

    def on_response(receipt):
        parsed = parse_heap(receipt.response)
        if parsed:
            sample.update(parsed)
            sample["ok"] = True
        state["done"] = True

    def established(lk):
        sample["stage"] = "request"
        lk.identify(identity)
        lk.request("/page/stack.mu", data={"var_c": category},
                   response_callback=on_response,
                   failed_callback=lambda _r: state.update(done=True))

    link.set_link_established_callback(established)
    deadline = time.time() + timeout
    while not state["done"] and time.time() < deadline:
        time.sleep(0.05)
    try:
        link.teardown()
    except Exception:
        pass
    return sample


def plot(path):
    with open(path, encoding="utf-8") as handle:
        rows = [json.loads(line) for line in handle if line.strip()]
    if not rows:
        sys.exit("no samples in %s" % path)
    start = min(r["at"] for r in rows)
    names = sorted({r["target"] for r in rows})
    for name in names:
        mine = [r for r in rows if r["target"] == name
                and r.get("category", "heap") == "heap"]
        ok = [r for r in mine if r.get("ok")]
        print("\n=== %s: %d samples, %d answered ===" % (name, len(mine), len(ok)))
        if not ok:
            continue
        print("%8s  %10s  %10s  %10s  %6s" % (
            "hours", "free", "min-free", "largest", "frag%"))
        for row in ok:
            print("%8.2f  %10d  %10d  %10d  %6s" % (
                (row["at"] - start) / 3600.0, row.get("heap_free", 0),
                row.get("heap_minfree", 0), row.get("heap_maxalloc", 0),
                row.get("heap_fragmented", "-")))
        first, last = ok[0], ok[-1]
        hours = (last["at"] - first["at"]) / 3600.0
        if hours > 0.5:
            for field in ("heap_free", "heap_maxalloc"):
                delta = last.get(field, 0) - first.get(field, 0)
                print("  %s: %+d bytes over %.1f h (%+.0f bytes/hour)" % (
                    field, delta, hours, delta / hours))
        missed = [r for r in mine if not r.get("ok")]
        if missed:
            first_miss = (missed[0]["at"] - start) / 3600.0
            print("  stopped answering %d times, first at %.2f h"
                  % (len(missed), first_miss))


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--target", action="append", default=[],
                        metavar="NAME=HASH")
    parser.add_argument("--interval", type=float, default=600.0,
                        help="seconds between rounds (default 600)")
    parser.add_argument("--hours", type=float, default=48.0)
    parser.add_argument("--timeout", type=float, default=40.0)
    parser.add_argument("--out", default=os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "..", "bench-runs",
        "heap-soak.jsonl"))
    parser.add_argument("--identity", default="~/.nomadnetwork/storage/identity")
    parser.add_argument("--pool", action="store_true",
                        help="also sample the TLSF arena each round")
    parser.add_argument("--plot", metavar="FILE")
    args = parser.parse_args()

    if args.plot:
        plot(args.plot)
        return 0
    if not args.target:
        sys.exit("no targets given\n" + parser.format_usage())

    targets = []
    for entry in args.target:
        name, _, hexhash = entry.partition("=")
        targets.append((name, bytes.fromhex(hexhash)))

    RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"),
                  loglevel=int(os.environ.get("RNSLL", "2")))
    identity = RNS.Identity.from_file(os.path.expanduser(args.identity))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    end = time.time() + args.hours * 3600.0
    print("sampling %d targets every %.0fs for %.1fh -> %s"
          % (len(targets), args.interval, args.hours, args.out))

    with open(args.out, "a", encoding="utf-8") as handle:
        while time.time() < end:
            for name, dest_hash in targets:
                # Both views each round: the system heap is where a node dies,
                # the arena is where we can see why. A pool that is barely
                # fragmented while the heap around it is not tells a very
                # different story from both degrading together.
                samples = [poll(name, dest_hash, identity, args.timeout, "heap")]
                if args.pool:
                    samples.append(poll(name, dest_hash, identity,
                                        args.timeout, "pool"))
                for sample in samples:
                    handle.write(json.dumps(sample) + "\n")
                handle.flush()
                sample = samples[0]
                print("%s %-7s %s" % (
                    time.strftime("%H:%M:%S"), name,
                    ("free=%d largest=%d frag=%s%%" % (
                        sample.get("heap_free", 0),
                        sample.get("heap_maxalloc", 0),
                        sample.get("heap_fragmented", "?")))
                    if sample.get("ok") else "NO ANSWER (%s)"
                    % sample.get("stage", "?")))
            time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
