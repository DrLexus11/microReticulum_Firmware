#!/usr/bin/env python3
# Interop test: real Python LXMF clients against the ESP32 propagation node.
# Sender A pushes a message for recipient B (never online), then B downloads it.
import os, sys, time
import RNS, LXMF

USAGE = """usage: interop.py <propagation_node_hash>

Full round trip against a propagation node: sender A pushes a message for
recipient B while B is offline, then B syncs and reads it back.

  LXBODY_PREFIX=<long string>   force the Resource path instead of the packet
                                path (payloads over ~319 bytes of content)
"""

if len(sys.argv) < 2:
    sys.exit(USAGE)
try:
    PN_HASH = bytes.fromhex(sys.argv[1])
except ValueError:
    sys.exit("error: propagation node hash must be hex\n\n" + USAGE)
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
BASE = os.environ.get(
    "LXMF_STATE_DIR",
    os.path.abspath(os.path.join(SCRIPT_DIR, "..", "..", "state", "lxmf-interop")),
)
os.makedirs(BASE, exist_ok=True)
SEND_TIMEOUT = float(os.environ.get("LXMF_SEND_TIMEOUT", "240"))
SYNC_TIMEOUT = float(os.environ.get("LXMF_SYNC_TIMEOUT", "180"))

RNS.loglevel = RNS.LOG_INFO
r = RNS.Reticulum(configdir=os.environ.get("RNS_CONFIGDIR"))

def mkid(name):
    p = f"{BASE}/{name}.id"
    if os.path.isfile(p): return RNS.Identity.from_file(p)
    i = RNS.Identity(); i.to_file(p); return i

id_a, id_b = mkid("a"), mkid("b")
print(f"A identity {RNS.prettyhexrep(id_a.hash)}")
print(f"B identity {RNS.prettyhexrep(id_b.hash)}")

# --- path to the propagation node ---
if not RNS.Transport.has_path(PN_HASH):
    print("requesting path to propagation node...")
    RNS.Transport.request_path(PN_HASH)
    for _ in range(150):
        if RNS.Transport.has_path(PN_HASH): break
        time.sleep(0.2)
if not RNS.Transport.has_path(PN_HASH):
    print("FAIL: no path to propagation node"); sys.exit(1)
print(f"path ok, {RNS.Transport.hops_to(PN_HASH)} hop(s)")

app_data = RNS.Identity.recall_app_data(PN_HASH)
seeded_app_data = os.environ.get("LXMF_PN_APP_DATA_HEX")
if app_data is None and seeded_app_data:
    try:
        app_data = bytes.fromhex(seeded_app_data)
        known = RNS.Identity.known_destinations[PN_HASH]
        RNS.Identity.remember(known[1], PN_HASH, known[2], app_data)
        print("using supplied propagation announce metadata")
    except (ValueError, KeyError, TypeError) as error:
        print(f"FAIL: invalid LXMF_PN_APP_DATA_HEX: {error}"); sys.exit(1)
if app_data is None:
    print("path has no cached announce data; waiting for a fresh propagation announce...")
    deadline = time.time() + 90
    while app_data is None and time.time() < deadline:
        time.sleep(0.5)
        app_data = RNS.Identity.recall_app_data(PN_HASH)
if app_data is None:
    print("FAIL: no announce app_data recalled -- cannot learn stamp cost"); sys.exit(1)
from RNS.vendor import umsgpack as msgpack
print("announced PN config:", msgpack.unpackb(app_data))
print("valid PN per LXMF:", LXMF.LXMRouter.pn_announce_data_is_valid(app_data)
      if hasattr(LXMF.LXMRouter,'pn_announce_data_is_valid') else
      LXMF.pn_announce_data_is_valid(app_data))

# --- sender ---
ra = LXMF.LXMRouter(identity=id_a, storagepath=f"{BASE}/a")
ra.register_delivery_identity(id_a, display_name="A")
ra.set_outbound_propagation_node(PN_HASH)

dest_b = RNS.Destination(id_b, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
src_a  = RNS.Destination(id_a, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")

state = {"done": None}
def delivered(m): state["done"] = "DELIVERED"
def failed(m):    state["done"] = "FAILED"

body = os.environ.get("LXBODY_PREFIX","interop test ") + RNS.hexrep(os.urandom(4), delimit=False)
msg = LXMF.LXMessage(dest_b, src_a, body, "test", desired_method=LXMF.LXMessage.PROPAGATED)
msg.delivery_callbacks = []
msg.register_delivery_callback(delivered)
msg.register_failed_callback(failed)
print(f"\n--- sending propagated message: {body!r} ---")
ra.handle_outbound(msg)

t0 = time.time()
while state["done"] is None and time.time()-t0 < SEND_TIMEOUT:
    time.sleep(0.5)
rep = {1: "PACKET", 2: "RESOURCE"}.get(msg.representation, msg.representation)
print(f"representation: {rep}  packed size: {len(msg.propagation_packed) if msg.propagation_packed else '?'}")
print(f"SEND RESULT: {state['done']} after {time.time()-t0:.1f}s  (transient_id "
      f"{RNS.prettyhexrep(msg.transient_id) if msg.transient_id else None})")
if state["done"] != "DELIVERED":
    print("FAIL: node did not acknowledge the propagated message"); sys.exit(1)

# --- recipient downloads ---
print("\n--- recipient B syncing from the node ---")
rb = LXMF.LXMRouter(identity=id_b, storagepath=f"{BASE}/b")
got = []
rb.register_delivery_identity(id_b, display_name="B")
rb.register_delivery_callback(lambda m: got.append(m))
rb.set_outbound_propagation_node(PN_HASH)
rb.request_messages_from_propagation_node(id_b)

t0 = time.time()
while not got and time.time()-t0 < SYNC_TIMEOUT:
    time.sleep(0.5)
    if rb.propagation_transfer_state == LXMF.LXMRouter.PR_COMPLETE and not got:
        time.sleep(3); break

if got:
    m = got[0]
    print(f"\nRECEIVED: title={m.title_as_string()!r} content={m.content_as_string()!r}")
    print("PASS" if m.content_as_string() == body else "FAIL: content mismatch")
else:
    print(f"FAIL: nothing downloaded (state={rb.propagation_transfer_state})")
