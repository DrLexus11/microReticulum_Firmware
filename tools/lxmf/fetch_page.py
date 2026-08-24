#!/usr/bin/env python3
import sys, time, RNS
RNS.loglevel = RNS.LOG_WARNING
r = RNS.Reticulum()
USAGE = """usage: fetch_page.py <destination_hash> [page_path]

Fetch a NomadNet page from a node. Useful for checking a board is alive over
the network rather than over serial. Default page is /page/index.mu.
"""

if len(sys.argv) < 2:
    sys.exit(USAGE)
try:
    H = bytes.fromhex(sys.argv[1])
except ValueError:
    sys.exit("error: destination hash must be hex\n\n" + USAGE)
PATH = sys.argv[2] if len(sys.argv) > 2 else "/page/index.mu"
if not RNS.Transport.has_path(H):
    RNS.Transport.request_path(H)
    for _ in range(100):
        if RNS.Transport.has_path(H): break
        time.sleep(0.2)
if not RNS.Transport.has_path(H):
    print("NO PATH"); sys.exit(2)
ident = RNS.Identity.recall(H)
if ident is None: print("NO IDENTITY"); sys.exit(2)
dest = RNS.Destination(ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "nomadnetwork", "node")
st = {"d": None}
link = RNS.Link(dest)
def ready(l):
    l.request(PATH, None,
              response_callback=lambda rr: st.update(d=rr.response),
              failed_callback=lambda rr: st.update(d="REQUEST FAILED"))
link.set_link_established_callback(ready)
t0=time.time()
while st["d"] is None and time.time()-t0 < 60: time.sleep(0.3)
d = st["d"]
if d is None: print("TIMEOUT (link status %s)" % link.status)
elif isinstance(d, str): print(d)
else: print(d.decode("utf-8","replace") if isinstance(d,(bytes,bytearray)) else d)
