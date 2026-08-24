#!/usr/bin/env python3
# Push a message for the Android client into a chosen propagation node's store,
# then stop. The point is that nothing is online to receive it directly.
import os, sys, time
import RNS, LXMF

USAGE = """usage: send_to_columba.py <propagation_node_hash> <delivery_dest_hash> [body]

Push one message for a real client into a node's store and stop, so the client
can sync it later. Both hashes are 16-byte hex.
"""

if len(sys.argv) < 3:
    sys.exit(USAGE)
try:
    PN   = bytes.fromhex(sys.argv[1])
    DEST = bytes.fromhex(sys.argv[2])
except ValueError:
    sys.exit("error: hashes must be hex\n\n" + USAGE)
BODY = sys.argv[3] if len(sys.argv) > 3 else "stored while you were away"
BASE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.abspath(os.path.join(BASE, "..", "..", "state", "lxmf-columba-sender"))
os.makedirs(STATE, exist_ok=True)

RNS.loglevel = RNS.LOG_WARNING
r = RNS.Reticulum()

p = os.path.join(STATE, "sender.id")
ident = RNS.Identity.from_file(p) if os.path.isfile(p) else RNS.Identity()
if not os.path.isfile(p): ident.to_file(p)

for h in (PN, DEST):
    if not RNS.Transport.has_path(h):
        RNS.Transport.request_path(h)
        for _ in range(150):
            if RNS.Transport.has_path(h): break
            time.sleep(0.2)

recipient_identity = RNS.Identity.recall(DEST)
if recipient_identity is None:
    print("FAIL: cannot recall recipient identity -- needs an announce from the phone")
    sys.exit(1)

router = LXMF.LXMRouter(identity=ident, storagepath=os.path.join(STATE, "router"))
router.register_delivery_identity(ident, display_name="Deck")
router.set_outbound_propagation_node(PN)

dest = RNS.Destination(recipient_identity, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
src  = RNS.Destination(ident, RNS.Destination.OUT, RNS.Destination.SINGLE, "lxmf", "delivery")
router.announce(src.hash)

state = {"r": None}
msg = LXMF.LXMessage(dest, src, BODY, "stored-and-forwarded",
                     desired_method=LXMF.LXMessage.PROPAGATED)
msg.register_delivery_callback(lambda m: state.update(r="ACCEPTED BY NODE"))
msg.register_failed_callback(lambda m: state.update(r="FAILED"))

print(f"sender    <{RNS.hexrep(src.hash, delimit=False)}> 'Deck'")
print(f"recipient <{RNS.hexrep(DEST, delimit=False)}>")
print(f"via node  <{RNS.hexrep(PN, delimit=False)}>")
print(f"body      {BODY!r}")
router.handle_outbound(msg)

t0 = time.time()
while state["r"] is None and time.time()-t0 < 240:
    time.sleep(0.5)
print(f"RESULT: {state['r']} after {time.time()-t0:.1f}s")
print(f"transient_id {RNS.hexrep(msg.transient_id, delimit=False) if msg.transient_id else None}")
time.sleep(3)
