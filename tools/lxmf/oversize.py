#!/usr/bin/env python3
"""Push an oversize Resource at the propagation node and check it is refused.

The node advertises a 64 KB sync limit. Before the guard, it accepted a Resource
of any size, which is the one thing a stranger can make it allocate for.
"""
import os, sys, time
import RNS

USAGE = """usage: oversize.py <propagation_node_hash> [bytes]

Push a Resource of a given size at the propagation destination. Used to check
that the size guard refuses oversize transfers, and to measure what the board
can actually receive. Default 200000 bytes, well over the advertised limit.
"""

if len(sys.argv) < 2:
    sys.exit(USAGE)
try:
    PN = bytes.fromhex(sys.argv[1])
except ValueError:
    sys.exit("error: propagation node hash must be hex\n\n" + USAGE)
try:
    SIZE = int(sys.argv[2]) if len(sys.argv) > 2 else 200_000
except ValueError:
    sys.exit("error: size must be an integer number of bytes\n\n" + USAGE)
RNS.loglevel = RNS.LOG_WARNING
r = RNS.Reticulum()

if not RNS.Transport.has_path(PN):
    RNS.Transport.request_path(PN)
    for _ in range(150):
        if RNS.Transport.has_path(PN): break
        time.sleep(0.2)
if not RNS.Transport.has_path(PN):
    sys.exit(f"no path to {PN.hex()} -- is the node up and announcing?")

# A known path does not imply a known identity: the destination can be routable
# while its public key is not cached, and passing None into Destination() fails
# somewhere far less obvious than here.
ident = RNS.Identity.recall(PN)
if ident is None:
    sys.exit(f"no identity cached for {PN.hex()} -- wait for an announce "
             f"(tools/lxmf/watch_announces.py) and retry")

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
