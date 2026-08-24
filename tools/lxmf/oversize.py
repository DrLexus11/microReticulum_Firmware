#!/usr/bin/env python3
"""Push an oversize Resource at the propagation node and check it is refused.

The node advertises a 64 KB sync limit. Before the guard, it accepted a Resource
of any size, which is the one thing a stranger can make it allocate for.
"""
import os, sys, time
import RNS

PN = bytes.fromhex(sys.argv[1])
SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 200_000
RNS.loglevel = RNS.LOG_WARNING
r = RNS.Reticulum()

if not RNS.Transport.has_path(PN):
    RNS.Transport.request_path(PN)
    for _ in range(150):
        if RNS.Transport.has_path(PN): break
        time.sleep(0.2)
ident = RNS.Identity.recall(PN)
dest = RNS.Destination(ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "propagation")

state = {"link": None, "res": None, "done": False}
def established(link):
    state["link"] = link
    payload = os.urandom(SIZE)          # incompressible, so no size cheating
    print(f"sending a {len(payload)} byte resource (limit is 64 KB)...", flush=True)
    def concluded(res):
        state["res"] = res.status; state["done"] = True
    state["r"] = RNS.Resource(payload, link, callback=concluded, auto_compress=False)

link = RNS.Link(dest, established_callback=established)
t0 = time.time()
while not state["done"] and time.time()-t0 < 180:
    time.sleep(0.5)

names = {v: k for k, v in vars(RNS.Resource).items() if isinstance(v, int)}
print(f"resource outcome: {names.get(state['res'], state['res'])} after {time.time()-t0:.1f}s")
print(f"link status after: {link.status}")

# The node must still be answering afterwards -- a refusal must not be a crash.
time.sleep(3)
probe = RNS.Link(dest)
t1 = time.time()
while probe.status != RNS.Link.ACTIVE and time.time()-t1 < 60:
    time.sleep(0.5)
print("node still reachable afterwards:", probe.status == RNS.Link.ACTIVE)
