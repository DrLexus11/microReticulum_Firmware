#!/usr/bin/env python3
"""Repeat the link+request probe and summarise outcomes.

Runs in one Reticulum process so the shared instance is not repeatedly
reattached. Reports per-attempt stage reached, so establishment failures and
request failures can be told apart.
"""

import argparse
import os
import sys
import time

import RNS

STAGE_NAMES = {0: "no-path", 1: "path", 2: "established", 3: "identified", 4: "response"}


def attempt(dest_hash, path, est_timeout, req_timeout, identity):
    result = {"stage": 0, "rtt": None, "mtu": None, "reason": None}
    t0 = time.time()

    if not RNS.Transport.has_path(dest_hash):
        RNS.Transport.request_path(dest_hash)
        while not RNS.Transport.has_path(dest_hash):
            if time.time() - t0 > 30:
                result["reason"] = "no path"
                return result
            time.sleep(0.1)
    result["stage"] = 1

    identity_obj = RNS.Identity.recall(dest_hash)
    if identity_obj is None:
        result["reason"] = "identity not recalled"
        return result

    destination = RNS.Destination(identity_obj, RNS.Destination.OUT,
                                  RNS.Destination.SINGLE, "nomadnetwork", "node")
    state = {"done": False}

    def on_response(receipt):
        result["stage"] = 4
        result["bytes"] = len(receipt.response) if receipt.response else 0
        state["done"] = True

    def on_failed(receipt):
        result["reason"] = "request failed"
        state["done"] = True

    link = RNS.Link(destination)

    def established(lk):
        result["stage"] = 2
        result["rtt"] = lk.rtt
        result["mtu"] = lk.mtu
        lk.identify(identity)
        result["stage"] = 3
        lk.request(path, data=None, response_callback=on_response,
                   failed_callback=on_failed)

    def closed(lk):
        if not state["done"]:
            result["reason"] = "link closed (teardown=%s)" % lk.teardown_reason
            state["done"] = True

    link.set_link_established_callback(established)
    link.set_link_closed_callback(closed)

    deadline = time.time() + est_timeout + req_timeout
    while not state["done"] and time.time() < deadline:
        time.sleep(0.1)
    if not state["done"]:
        result["reason"] = "timeout at stage %s" % STAGE_NAMES.get(result["stage"])
    try:
        link.teardown()
    except Exception:
        pass
    result["elapsed"] = time.time() - t0
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dest")
    ap.add_argument("--path", default="/page/index.mu")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--est-timeout", type=float, default=25.0)
    ap.add_argument("--req-timeout", type=float, default=45.0)
    ap.add_argument("--gap", type=float, default=5.0)
    ap.add_argument("--loglevel", type=int, default=3)
    args = ap.parse_args()

    RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"), loglevel=args.loglevel)
    identity = RNS.Identity.from_file(
        os.path.expanduser("~/.nomadnetwork/storage/identity"))
    dest_hash = bytes.fromhex(args.dest)

    results = []
    for i in range(args.runs):
        r = attempt(dest_hash, args.path, args.est_timeout, args.req_timeout, identity)
        results.append(r)
        print("run %d/%d stage=%-11s rtt=%s mtu=%s elapsed=%.1fs %s"
              % (i + 1, args.runs, STAGE_NAMES.get(r["stage"], "?"),
                 ("%.2f" % r["rtt"]) if r.get("rtt") else "-",
                 r.get("mtu") or "-", r.get("elapsed", 0),
                 r.get("reason") or "OK bytes=%s" % r.get("bytes")),
              flush=True)
        if i + 1 < args.runs:
            time.sleep(args.gap)

    print("\nsummary for %s %s" % (args.dest, args.path))
    for stage in sorted(STAGE_NAMES):
        n = sum(1 for r in results if r["stage"] == stage)
        if n:
            print("  reached %-11s : %d" % (STAGE_NAMES[stage], n))
    return 0


if __name__ == "__main__":
    sys.exit(main())
