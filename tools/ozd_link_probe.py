#!/usr/bin/env python3
"""Open a Reticulum link to a node destination and request a page.

Diagnostic for the OZD-ARD-01 ESP-NOW fixture: reports each establishment
stage with timing so a failure can be attributed to path resolution, link
establishment, or the request itself.
"""

import argparse
import os
import sys
import time

import RNS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dest", help="destination hash (hex)")
    ap.add_argument("--path", default="/page/index.mu")
    ap.add_argument("--timeout", type=float, default=90.0)
    ap.add_argument("--config", default=os.path.expanduser("~/.reticulum"))
    args = ap.parse_args()

    try:
        dest_hash = bytes.fromhex(args.dest)
    except ValueError:
        print("destination must be hex")
        return 2
    if len(dest_hash) != RNS.Reticulum.TRUNCATED_HASHLENGTH // 8:
        print("destination must be %d bytes" % (RNS.Reticulum.TRUNCATED_HASHLENGTH // 8))
        return 2

    RNS.Reticulum(configdir=args.config, loglevel=args.__dict__.get("loglevel", 4))

    t0 = time.time()
    if not RNS.Transport.has_path(dest_hash):
        print("[%6.2f] no path, requesting..." % 0.0)
        RNS.Transport.request_path(dest_hash)
        while not RNS.Transport.has_path(dest_hash):
            if time.time() - t0 > args.timeout:
                print("[%6.2f] FAIL: path never resolved" % (time.time() - t0))
                return 1
            time.sleep(0.1)
    print("[%6.2f] path ok, %d hops, next hop iface %s"
          % (time.time() - t0,
             RNS.Transport.hops_to(dest_hash),
             RNS.Transport.next_hop_interface(dest_hash)))

    identity = RNS.Identity.recall(dest_hash)
    if identity is None:
        print("[%6.2f] FAIL: identity not recalled" % (time.time() - t0))
        return 1

    destination = RNS.Destination(
        identity, RNS.Destination.OUT, RNS.Destination.SINGLE,
        "nomadnetwork", "node")

    state = {"done": False, "ok": False}

    def established(link):
        print("[%6.2f] LINK ESTABLISHED rtt=%.3fs mtu=%s mdu=%s"
              % (time.time() - t0, link.rtt or -1, link.mtu, link.mdu))
        link.identify(RNS.Identity.from_file(
            os.path.expanduser("~/.nomadnetwork/storage/identity")))
        print("[%6.2f] identified, requesting %s" % (time.time() - t0, args.path))
        link.request(args.path, data=None,
                     response_callback=on_response,
                     failed_callback=on_failed,
                     progress_callback=on_progress)

    def closed(link):
        print("[%6.2f] LINK CLOSED teardown_reason=%s status=%s"
              % (time.time() - t0, link.teardown_reason, link.status))
        state["done"] = True

    def on_response(receipt):
        resp = receipt.response
        print("[%6.2f] RESPONSE %d bytes" % (time.time() - t0,
                                             len(resp) if resp else 0))
        if resp:
            text = resp.decode("utf-8", "replace") if isinstance(resp, bytes) else str(resp)
            print("---8<---")
            print(text[:2000])
            print("--->8---")
        state["ok"] = True
        state["done"] = True

    def on_failed(receipt):
        print("[%6.2f] REQUEST FAILED" % (time.time() - t0))
        state["done"] = True

    def on_progress(receipt):
        print("[%6.2f]   progress %.1f%%" % (time.time() - t0,
                                             (receipt.progress or 0) * 100))

    print("[%6.2f] opening link..." % (time.time() - t0))
    link = RNS.Link(destination)
    link.set_link_established_callback(established)
    link.set_link_closed_callback(closed)

    last_status = None
    while not state["done"]:
        if link.status != last_status:
            print("[%6.2f]   link.status=%s" % (time.time() - t0, link.status))
            last_status = link.status
        if time.time() - t0 > args.timeout:
            print("[%6.2f] TIMEOUT, final status=%s" % (time.time() - t0, link.status))
            break
        time.sleep(0.1)

    try:
        link.teardown()
    except Exception:
        pass
    time.sleep(0.5)
    return 0 if state["ok"] else 1


if __name__ == "__main__":
    sys.exit(main())
