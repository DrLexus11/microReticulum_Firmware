#!/usr/bin/env python3
"""Fetch a single page and print the raw response content."""
import os
import sys
import time

import RNS

dest_hex = sys.argv[1]
path = sys.argv[2]
category = sys.argv[3] if len(sys.argv) > 3 else None
request_data = {"var_c": category} if category else None

RNS.Reticulum(configdir=os.path.expanduser("~/.reticulum"), loglevel=int(os.environ.get("RNSLL","2")))
identity = RNS.Identity.from_file(
    os.path.expanduser("~/.nomadnetwork/storage/identity"))
print("client identity hash:", RNS.hexrep(identity.hash, delimit=False))

dest_hash = bytes.fromhex(dest_hex)
if not RNS.Transport.has_path(dest_hash):
    RNS.Transport.request_path(dest_hash)
    t0 = time.time()
    while not RNS.Transport.has_path(dest_hash) and time.time() - t0 < 30:
        time.sleep(0.1)

identity_obj = RNS.Identity.recall(dest_hash)
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
