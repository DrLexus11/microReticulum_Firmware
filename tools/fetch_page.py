#!/usr/bin/env python3
"""Fetch a single page and print the raw response content."""
import os
import sys
import time

import RNS

USAGE = "usage: fetch_page.py <destination-hash-hex> <page-path> [category]"

if len(sys.argv) < 3:
    sys.exit(USAGE)

dest_hex = sys.argv[1]
path = sys.argv[2]
category = sys.argv[3] if len(sys.argv) > 3 else None
request_data = {"var_c": category} if category else None

# Validate before touching the network. Each of these failures used to surface
# as a stack trace or, worse, as a silent timeout indistinguishable from a node
# that is simply not answering -- which cost real time while diagnosing a board
# that turned out to be denying the request rather than ignoring it.
try:
    dest_hash = bytes.fromhex(dest_hex)
except ValueError:
    sys.exit(f"not a hex destination hash: {dest_hex!r}\n{USAGE}")

if len(dest_hash) != RNS.Reticulum.TRUNCATED_HASHLENGTH // 8:
    sys.exit(
        f"destination hash is {len(dest_hash)} bytes, expected "
        f"{RNS.Reticulum.TRUNCATED_HASHLENGTH // 8}: {dest_hex!r}"
    )

RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"), loglevel=int(os.environ.get("RNSLL","2")))
identity = RNS.Identity.from_file(
    os.path.expanduser("~/.nomadnetwork/storage/identity"))
print("client identity hash:", RNS.hexrep(identity.hash, delimit=False))

if not RNS.Transport.has_path(dest_hash):
    RNS.Transport.request_path(dest_hash)
    t0 = time.time()
    while not RNS.Transport.has_path(dest_hash) and time.time() - t0 < 30:
        time.sleep(0.1)

if not RNS.Transport.has_path(dest_hash):
    sys.exit(f"no path to {dest_hex} after 30s -- the destination has not "
             "announced, or nothing on this instance can reach it")

# Without this the Destination is built with identity None, and the failure
# surfaces much later as an unrelated exception or a hang.
identity_obj = RNS.Identity.recall(dest_hash)
if identity_obj is None:
    sys.exit(f"a path to {dest_hex} exists but its identity is not known -- "
             "wait for an announce, or check this is a SINGLE destination")

destination = RNS.Destination(identity_obj, RNS.Destination.OUT,
                              RNS.Destination.SINGLE, "nomadnetwork", "node")
state = {"done": False}


def on_response(receipt):
    r = receipt.response
    print("RESPONSE type=%s len=%s" % (type(r).__name__, len(r) if r else 0))
    print("REPR:", repr(r)[:600])
    state["done"] = True


def on_failed(receipt):
    print("REQUEST FAILED")
    state["done"] = True


link = RNS.Link(destination)


def established(lk):
    print("established rtt=%.2f mtu=%s" % (lk.rtt, lk.mtu))
    lk.identify(identity)
    lk.request(path, data=request_data, response_callback=on_response,
               failed_callback=on_failed)


link.set_link_established_callback(established)
link.set_link_closed_callback(
    lambda lk: print("closed teardown=%s" % lk.teardown_reason))

deadline = time.time() + 60
while not state["done"] and time.time() < deadline:
    time.sleep(0.1)
if not state["done"]:
    print("TIMEOUT")
link.teardown()
time.sleep(0.5)
