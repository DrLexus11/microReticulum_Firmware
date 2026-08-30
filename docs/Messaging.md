# Messaging: Why It Failed, and the Store-and-Forward Decision

Why NomadNet pages were rock solid overnight while messaging was not, what the
missing component is, and whether to implement a propagation node on the ESP32.

Status: **Linux propagation node running as of 2026-08-24. ESP32 propagation
node implemented the same day, verified serving a real Android client, and
hardware-tested through multi-message sync and the 128-message eviction
boundary** -- see §§7–10. Findings below are from an overnight two-room test
unless marked otherwise.

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
the link.

**Updated: `/offer` is now accepted.** It previously declined every offer, and
deliberately so -- accepting meant taking a Linux node's 500 MB backlog into a
512 KB flash store and evicting the local residents' messages that are the entire
point of the node.

What changed is the store, not the risk assessment. Peer-received messages now
occupy a bounded share of it (`LXMF_PN_PEER_SHARE_PCT`, 50% by default) and are
evicted before any local message. That is a guarantee about our own store rather
than a claim about the peer's, so it holds whatever is on the other end -- which
is what `autopeer_maxdepth` alone cannot do, since a large peer one hop away is
inside any depth bound.

The node asks only for ids it does not already hold, and only as many as its
share has room for. Asking for fewer than were offered is normal in LXMF; the
peer keeps the rest.

**Still outstanding: the outbound half.** This node accepts offers but never
makes them -- it has no peer discovery and never calls `/offer` on anyone. A
Linux `lxmd` that offers to us will now sync into us, but two RADs will not
converge on their own, because neither initiates. That is the remaining work for
roadmap item 4a.

### What it does

| | |
| --- | --- |
| Announce | `lxmf.propagation`, advertising 4 KB per transfer, 8 KB per sync, stamp costs 16/3/18 |
| Receive | link packet **and** Resource, both proving or storing as appropriate |
| Store | LittleFS, capped at 128 messages / 512 KB; peer-received messages capped at half that and evicted before any local message |
| `/get` | list, download, and purge, all scoped to the requesting identity |
| `/offer` | accepts, bounded by a peer share of the store; outbound offers not yet implemented |

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

**Untested against Sideband and Reticchat specifically.** *Closed 2026-08-24 for
Columba on Android, including a conclusive LoRa-hop acceptance run -- see §8.
Reticchat is not currently available and remains follow-up client coverage, not
a blocker on the propagation-node protocol proven by stock Columba and LXMF.*

---

## 8. Verified against a real client

2026-08-24. Columba on Android retrieved a message that had been stored on an
ESP32 propagation node while the recipient was offline. This is the deliverable
the whole exercise was for, tested against the client a resident would actually
use rather than a Python stand-in.

The run that mattered: a message addressed to the phone was pushed into **Rev 2**
(`41fc2ab5...`) with the phone not fetching, then Columba -- attached to **Rev 1**
over TCP -- was pointed at Rev 2 and synced, and the message arrived.

### The control that made the result readable

An earlier sync returned nothing, which looked like our node failing. It was not.
Two messages were queued, one on our node and one on the Linux `lxmd`, with
distinct bodies; the phone received only the `lxmd` one. That isolated the cause
to **client configuration** -- Columba was still pointed at the Deck -- rather
than to the store-and-forward implementation, which had never been asked for
anything.

Worth keeping as a habit: when a client reports "nothing", the useful question is
not "is our node broken" but "which node did it actually ask". A second labelled
message through a known-good node answers it in one round trip.

### Independent confirmation of wire format

The Linux `lxmd` message store was inspected directly while holding a message
for the same phone:

```
b4243d19536572f5aec5b5f41a31638c  272 bytes  cc5d5c33..._1787...
```

The filename is the transient id and the first 16 bytes of the blob are the
destination hash -- exactly what our implementation derives, with the transient
id matching what the sending client computed. The stamp-stripping split in §7 is
therefore confirmed against Python's own on-disk representation, not just
inferred from reading its source.

### What this does and does not establish

**Established:** an ESP32 node announces itself so a real client will use it,
accepts a message for an absent recipient, stores it across the recipient being
offline, and serves it on demand to a phone running stock client software.

**Conclusive LoRa-hop acceptance, 2026-08-24:** Rev 2 was UART-flashed with the
final build, power-cycled, repaired with `fixhash`, and queried directly. Radio
state was `1`; target and running hashes both equalled
`577d00d8ece2c56ee2ad553114b81ebee85e9e6d71372fd08abc0f4b32ccb6dc`.
The Deck then stored `PR-RF-ACCEPT REV2->LEXUS 2026-08-24 run-01` on Rev 2 for
Lexus/Columba while Columba was attached to Rev 1 over TCP and configured to use
Rev 2 as its relay.

UART capture showed the complete transaction: `/get list ... 2 message(s)`,
`/get serving 2 of 2`, multiple `[radio] Sent ...` frames, then `/get purged 2`.
Across that retrieval Rev 2's driver-level `tx_calls` rose from 7 to 13 and its
preamble/header counters rose to 17/10, while the radio stayed online with
`hw_ready=1` and no error. Columba displayed the exact labelled body and its
acknowledgement removed it from the store. This proves stock-client
store-and-forward across the Rev 1↔Rev 2 RF hop rather than merely inferring the
route from path length.

**Still untested:** Reticchat on iOS, which is not currently available.
Multi-message truncation and full-store eviction are covered in §10; Reticchat
is retained as follow-up client-matrix coverage.

## 9. A flashing hazard worth knowing about

Found the hard way, and fixed in `extra_script.py` on 2026-08-24.

After an upload the build script writes the firmware's SHA-256 into EEPROM over
KISS. `device_init()` compares that against the running partition, and a mismatch
means `hw_ready = 0` and **the radio never starts**. The `-uart` environments
upload with `--after=no_reset`, so the board is still in the bootloader when that
write happens: it never lands, and the stored hash stays at the *previous*
firmware's value.

The failure is nasty because it does not look like a radio failure. The node
boots, joins over WiFi, announces, and serves NomadNet pages perfectly, while
being completely deaf and mute on RF -- with nothing in the log to say why. It
cost most of an afternoon, and the thing that finally identified it was forcing
`startRadio()` to explain itself over KISS:

```
[radio] startRadio BLOCKED locked=0 hwr=0
```

Three changes came out of it:

- The post-upload step now **detects** an env that leaves the board in the
  bootloader and refuses to pretend it wrote the hash, printing what will happen
  and the exact command to fix it.
- A `fixhash` target writes the built firmware's hash to an already-running
  board, for use after power-cycling out of download mode, and now issues the
  KISS reboot itself so `device_init()` immediately re-validates the hash.
- The SX126x `[lora]` diagnostic now prints `locked/hwr/err/con/alock`, which the
  SX1276 branch already did. Their absence is precisely why this took source
  reading rather than log reading.

---

## 10. Hardening pass, 2026-08-24

Work done to take the feature from "demonstrated" to "defensible".

### The advertised sync limit was a promise the board could not keep

Testing the new size guard meant pushing oversized Resources at a node, which
incidentally measured what it can actually receive:

| Resource | Outcome |
| --- | --- |
| 8 KB | COMPLETE in 2.0 s |
| 16 KB | FAILED after 103.5 s (stalls, then times out) |
| 32 KB | FAILED after 104.5 s |
| 200 KB | refused in 12.5 s by the new guard |

The practical inbound ceiling is between 8 and 16 KB, against an advertised sync
limit of **64 KB**. Real traffic worked only because messages are small; a client
batching toward the advertised figure would have stalled for a minute and a half
and failed. Limits are now **4 KB per message, 8 KB per sync**, taken from the
measurement rather than from Python's defaults.

Two lessons worth keeping. First, an advertised limit is a contract, and one
nobody checks is a trap for whoever believes it. Second, this was found only
because a defensive change was tested for whether it *fires*, not just for
whether it breaks anything -- the oversize test was written to prove the guard
worked and found a pre-existing bug instead.

**Not yet understood:** why transfers stall above ~8 KB -- window sizing,
retries, or memory during reassembly. Advertising honestly does not depend on
the answer, but the ceiling is lower than the hardware ought to manage.

### Inbound Resources are now bounded

Previously the link used `ACCEPT_ALL`, so any peer could make the node allocate
for a Resource of any size -- the one remotely-triggerable memory risk in the
feature. A 200 KB push is now refused in ~12 s and the node stays reachable.

This is done by cancelling on transfer start, **not** with `ACCEPT_APP`. That
strategy exists for exactly this in Python, but in this C++ port the resource
callback returns `void` and `Link.cpp` accepts unconditionally afterwards
regardless:

```c
_object->_callbacks._resource(resource_advertisement);
// Currently the resource() callback returns void on the
// C++ port; accept unconditionally if a callback is set.
Resource::accept(packet, ...);
```

So `ACCEPT_APP` would read as a refusal that never refuses. `Resource::accept()`
does not preallocate, so cancelling at start bounds the cost to about one
window. **Follow-up:** make the callback's verdict count in microReticulum, which
is an upstream change to our own fork.

### Also in this pass

- **The feature is enabled by default** for both RAD environments. It previously
  existed only behind a flag no environment set, so a normal build shipped none
  of it. Flash is 84.9% (Rev 1) and 85.6% (Rev 2).
- **Per-sync total is enforced** during ingest, not just the per-message limit --
  otherwise a sender could stay under the message limit and still fill the store
  in one request.
- **Contract tests** in `tests/test_lxmf_protocol.py` (16 tests) parse the
  firmware's own `#define`s and assert them against the LXMF reference library:
  stamp size, destination length, message overhead, stamp costs, request paths
  and every error code. Since the failure mode here is silence, this fails loudly
  if we drift or if upstream changes. Run them under the RNS virtualenv; they
  skip cleanly without it.

### Multi-message sync and client-limit clamping

The first multi-message run exposed a protocol trap in `/get`. Stock Python
LXMF sends its own default delivery limit of **1000 KB** in the request. The
firmware treated that as a replacement for the node's 8 KB ceiling, so a client
could accidentally raise the limit and provoke the same oversized Resource
stall the announce was meant to prevent. The client value is now a one-way
constraint: it may lower the node limit, never raise it.

LXMF's reference code also defines the advertised kilobyte fields as decimal
kilobytes (`value * 1000`). Two firmware guards used 1024. The guards and their
contract tests now use 1000, so the on-wire promise and enforced byte count are
identical.

Hardware results on Rev 1:

| Run | Retrieval rounds | Result |
| --- | --- | --- |
| 3 messages, ordinary client | `3, 0` | PASS |
| 16 messages, client limit 2 KB | `6, 6, 4, 0` | PASS |
| 30 messages, stock 1000 KB client after final fix | `12, 12, 6, 0` | PASS |

All comparisons use deterministic bodies and require exact count and contents,
with no missing, unexpected or duplicate deliveries. The reusable harness is
`tools/lxmf/propagation_stress.py`.

### Full-store eviction and the outbound response budget

Rev 1 was filled with 129 deterministic 302-byte propagated messages. All 129
were acknowledged. Retrieval returned exactly messages 2 through 129: **128
expected, 128 received, oldest absent, zero missing/unexpected/duplicates**.
That proves the 128-message cap, oldest-first eviction, repeated `/get`, and
purge behavior on hardware.

The test also found a load-dependent edge. With all 128 index entries present,
stock-client retrieval packed near the 8 KB ceiling and three consecutive links
stalled while the board itself stayed reachable. Resuming the same preserved
identity at a 2 KB client limit completed in `6 × 21, 2, 0` rounds. The node now
caps outbound message responses at **4096 bytes**. This is large enough for one
maximum 4000-byte stored blob plus the conservative response overhead, but
leaves Resource and full-store memory headroom. Returning a partial wanted set
is normal LXMF behavior; the client requests the remainder in later rounds.

The complete 129-message test was then repeated on that final build with the
stock client's 1000 KB request. It passed without a retry as
`13 × 9, 11, 0`: exactly the newest 128 messages, with the oldest absent and no
missing, unexpected or duplicate bodies. This combines the eviction and
response-budget proofs under maximum index load.

The nominal 512 KiB byte cap cannot currently be the first cap reached by valid
traffic: `128 × 4000 = 512000`, below `512 × 1024 = 524288`. The count cap will
always fire first. The byte guard remains defense-in-depth for build overrides
or future limit changes. This is now an explicit contract, covered by an
invariant test; testing the eviction branch independently would require a
deliberately lower-capacity test build.

### Repeatable board and flash checks

`tools/rad01/lab_status.py` records the lab mapping and checks both stable serial
paths, port ownership, ICMP, TCP 4242 and recalled LXMF announce data without
opening either board's serial port. Rev 2's UART recovery was completed with
`fixhash`; its radio started, RNS/Transport became ready, and it announced the
current `4/8/[16,3,18]` contract. Rev 1's normal native-USB upload path was also
exercised through image verification, direct hash write, automatic KISS reboot,
and return to healthy RNS service.
