# Messaging: Why It Failed, and the Store-and-Forward Decision

Why NomadNet pages were rock solid overnight while messaging was not, what the
missing component is, and whether to implement a propagation node on the ESP32.

Status: **Linux propagation node running as of 2026-08-24. ESP32 propagation
node implemented and interop-verified the same day** -- see §7. Findings below
are from an overnight two-room test unless marked otherwise.

---

## 1. The overnight test

Rev 1 and Rev 2 in separate rooms, Deck asleep, Android (Columba) attached to
Rev 1 over TCP and iOS (Reticchat) to Rev 2. The two nodes have no IP path to
each other -- both UDP interfaces point at the sleeping Deck and neither is a
TCP client -- so everything between the phones crossed the radio.

**What worked, all night:**

- Rev 1's NomadNet pages, **under 1 second** over TCP.
- Rev 2's pages, reachable from a client attached to Rev 1, i.e. across the LoRa
  hop.
- The radio itself: Rev 2 logged `packets_sent 500 / received 484`, `last_rssi
  -47`, noise floor `-108` -- a healthy ratio at strong signal, across rooms.

**What did not:** messaging between the two phones. Some messages arrived, some
were marked *not delivered* by Reticchat, some vanished silently.

## 2. Why pages worked and messages did not

The difference is what sits at the far end.

- **A page fetch terminates on a node.** Always powered, never backgrounded,
  always reachable. Both endpoints reliable.
- **A message terminates on another phone.** With no propagation node in the
  network, LXMF can only deliver *opportunistically*: the sender opens a link
  directly to the recipient, which requires the recipient to be online and
  reachable **at that instant**.

iOS suspends backgrounded apps aggressively. Every time Reticchat was suspended
its RNS instance stopped, its link to Rev 2 dropped, and delivery failed. That
is exactly the observed pattern: success or failure depending purely on whether
the recipient happened to be awake.

Verified on the Deck at the time: `enable_node = no` in the NomadNet config, no
`lxmd` running, and no LXMF layer in the firmware at all. The clients were
already configured with `try_propagation_on_send_fail = yes` -- the software
tried to fall back to store-and-forward and found nothing to fall back to.

**In a disaster, simultaneous presence is precisely what you do not have.**
Phones are off, flat, in a pocket, in a stairwell. A messaging system that
requires both parties online at the same moment fails in the situation it exists
for. Store-and-forward is not an optimisation here; it is the feature.

## 3. No client can fix this

Worth stating plainly, because "write our own client" is the tempting answer.

iOS suspends background apps and restricts background networking as a matter of
platform policy. A bespoke client would be suspended identically. The only
architecture that works when an endpoint is asleep is store-and-forward, and
LXMF already has it.

The same reasoning rules out inventing a protocol: LXMF is what Sideband,
NomadNet and Reticchat already speak. A new protocol means writing every client,
on every platform, forever -- to obtain a feature the existing protocol has.

## 4. What was actually missing, and what fixed it

A propagation node. `lxmd -p` is now running on the Deck:

```
LXMF Propagation Node running on <208e0a36ba7a9b51581c9cda04b8da56>
Messagestore: 0 messages (0.0% of 500 MB)
Accepting propagated messages from all nodes
```

No firmware change, no client change, no protocol work. The network was missing
a component, not the software a capability.

**Product consequence: every blackbox runs a propagation node.** It is Linux,
mains-powered and always on, which is exactly what `lxmd` wants. One per
building; they peer, so a message can wait on the local roof and sync outward
when a path exists. This is a requirement, not a nice-to-have.

## 5. An ESP32 propagation node: recommended

The obvious objection to §4 is that it does not help a deployment with no Linux
box -- which includes the current lab. So: can a RAD hold messages itself?

**Yes, and it is more tractable than it first appears.** Measured, not assumed:

| | |
| --- | --- |
| RAD filesystem free | **1,638,400 bytes** (83% of 1.92 MB) -- thousands of short texts |
| `Resource.cpp` | **1,729 lines, 1 TODO** -- chunked transfer already works |
| Stamps | a PN only **validates** proof-of-work (one hash); senders generate it |

And the simplification that matters most: **a propagation node never decrypts
anything.** Messages are end-to-end encrypted, so the node stores opaque blobs
keyed by destination hash. No LXMessage parsing, no delivery logic, no stamp
generation. The scope is far smaller than "implement LXMF" (~4,500 lines in
Python, most of it client-side concerns):

1. Announce an `lxmf.propagation` destination with correct app data
2. Handle offers -- client lists message ids, node replies which it wants
3. Receive via Resource transfer *(already implemented)*
4. Validate stamps *(one sha256 check)*
5. Store to filesystem, keyed by destination hash
6. Serve sync requests
7. Expiry and limits

PN-to-PN peering can be omitted entirely in v1.

### Risks, in order

- **Bit-compatibility with Python LXMF.** The offer/sync protocol must match
  exactly or Sideband and Reticchat silently fail to sync. This is the same
  silent-failure signature that has cost this project the most time. **Interop
  test against real `lxmd`, not node to node.**
- **Advertise a smaller message limit.** Python defaults to 256 KB per message;
  one such message would eat a sixth of a RAD's store. An ESP node should
  advertise ~4-8 KB and be text-only. Limits travel in the announce, so this is
  supported rather than a hack.
- **Flash wear.** LittleFS on the app partition, continuously written. Wants a
  PSRAM cache and bounded write patterns, not naive per-message writes.
- **Airtime.** Sync over LoRa is expensive, and once duty cycle is enforced a
  node has ~36 s of transmit per hour. A LoRa-reachable PN is capacity-limited
  in a way a WiFi one is not.

### Why this outranks IFAC

A reversal of the ordering in `docs/PrivateMesh.md`, and the reasoning is worth
recording: **messaging unreliability is a demonstrated failure observed on
hardware; IFAC is an anticipated requirement for talks still at the talking
stage.** Evidence should outrank anticipation.

It also unlocks a **RAD-only building tier** with no Linux blackbox -- cheaper to
deploy, and more resilient, since in-flat nodes keep holding messages for each
other if the blackbox dies rather than the building going silent.

## 6. Order of work

1. **Prove the architecture** with the Linux propagation node now running.
   Specifically: send to the iOS device with Reticchat closed, then open it. If
   the message arrives on open, the diagnosis is confirmed.
2. **ESP32 propagation node**, scoped as §5. *Done -- see §7 for what the scope
   got wrong and how it was verified.*
3. **IFAC**, unless procurement talks harden first.

Step 1 exists to de-risk step 2. If store-and-forward turns out not to be the
fix, an ESP propagation node would have been the wrong thing to build.

**Caveat on step 1:** the Deck's propagation node is reachable over WiFi from
both phones, so tonight tests the LXMF layer but *not* propagation across the
LoRa hop. Testing that needs the node on the far side of the radio -- which is
what a rooftop blackbox will be, and deserves a deliberate test once one exists.

---

## 7. The ESP32 propagation node, as built

Implemented 2026-08-24 behind `-DLXMF_PROPAGATION_NODE`, in `LXMFPropagation.h`.
A client can push a message to a RAD and a different client can collect it
later, with neither online at the same moment.

### What reading the Python changed

The scope in §5 was close but wrong in three places, each found by reading
`LXMRouter.py` rather than inferring the protocol from the announce format. All
three would have produced silent failures rather than errors.

**Short messages are not Resources.** LXMF promotes a message to a Resource only
above 319 bytes of content; below that it goes as a plain packet on the link.
Ordinary texts are therefore *not* on the path §5 assumed was already done, and
a node handling only Resources would appear to work while quietly ignoring most
real traffic. Worse, the packet path must `prove()` the packet -- that proof is
what turns the sender's "sent" into "delivered", so omitting it reproduces
exactly the not-delivered symptom of §1. Both paths are now implemented and
share one ingest routine.

**Every message carries a stamp.** The sender appends a 32-byte proof-of-work
stamp sized to the cost we advertise. The transient id is the hash of the
message *without* the stamp, while the stored blob keeps it and `/get` strips it
again on the way out. Hash the wrong span and every id the node advertises is
one no client recognises -- messages that sync but never arrive.

**`/offer` is peer-to-peer only.** Clients never call it; they push straight onto
the link. §5 was right that peering could be dropped from v1, but the reason
matters more than expected: the node now *declines* offers deliberately rather
than for lack of implementation, because accepting means taking a Linux node's
500 MB backlog into a 512 KB flash store, evicting the local residents' messages
that are the entire point of the node.

### What it does

| | |
| --- | --- |
| Announce | `lxmf.propagation`, advertising 8 KB per transfer, 64 KB per sync, stamp costs 16/3/18 |
| Receive | link packet **and** Resource, both proving or storing as appropriate |
| Store | LittleFS, capped at 128 messages / 512 KB, oldest evicted first |
| `/get` | list, download, and purge, all scoped to the requesting identity |
| `/offer` | declines -- no peer sync in v1 |

Ownership on `/get` is checked against the delivery destination derived from the
identity proved on the link, not from anything in the request, so one client
cannot read or delete another's mail.

### Verification

Against **real Python LXMF clients**, which is what §5's risk list demanded --
node-to-node testing would have proven nothing about bit-compatibility. Sender A
pushes a message for recipient B while B is offline; B then syncs and reads it.

- Five round trips on the packet path (270-byte payloads), all delivered and
  read back with matching content.
- Two round trips on the Resource path (782-byte payloads). A payload that size
  cannot arrive as a link packet, so a successful round trip is itself proof the
  Resource handler ran.
- Node-side log confirms the full sequence: `client packet: 1 offered, 1 stored`
  -> `/get list ... 1 message(s)` -> `/get serving 1 of 1` -> `/get purged 1 ...
  0 held`.
- The announce parses as a valid propagation node under LXMF's own validator.

### Known gaps

**Stamps are stripped but not validated.** The node does not check the
proof-of-work. This is deliberately the permissive direction -- it never rejects
a legitimate message -- but it means the anti-spam cost we advertise is not
enforced, and a sender could append 32 arbitrary bytes. Validating costs one
1000-round HKDF workblock (256 KB, streamable) plus one hash per message, which
is affordable on an S3; the reason to defer it is that a one-byte divergence
from Python would silently reject real messages, so it wants a cross-checked
test vector before it goes in.

**The announce timebase is uptime, not wall time.** The board has no RTC. Nothing
observed depends on it -- clients accept the announce and sync correctly -- but
peer sync would, and it should be fixed by adopting time from a client request
before peering is ever implemented.

**No duty-cycle accounting for sync traffic.** Syncing over LoRa is expensive and
`RADIO_DUTY_CYCLE_LONGTERM` is still `0.0` for lab use. A LoRa-reachable
propagation node is capacity-limited in a way a WiFi one is not, and this
interacts with the shipping reminder in `Config.h`.

**Untested against Sideband and Reticchat specifically.** The Python LXMF library
is the same protocol implementation those clients use, so the risk is low, but
"low" is not "measured" -- and the phones are the actual product.
