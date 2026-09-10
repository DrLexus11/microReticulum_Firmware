# PR 2: authenticated tasking over the RAD mesh

Status: implemented; all merge gates met on hardware (2026-09-08, 2026-09-10).
Outdoor range and load remain the separate two-Rev2 exercise. Branch `feature/atak-tasking`, based
on `bbcc069` (merged position PR #21). Companion Columba branch:
`feature/tak-tasking`, based on `24b0a07f` from position reporting.

The first downlink should let a command-post operator send an addressed
go-to-point task with a short instruction. Columba presents the verified task
in a persistent inbox and lets its user accept or decline it. A task does not
start navigation or move a device automatically. Acceptance is a user decision,
distinct from packet delivery.

This is the next indoor PR. The infrastructure-free ESP-NOW/LoRa exercise
remains outdoor PR A and must pass before claiming field readiness. Markers
and LXMF/GeoChat bridging follow; voice and on-board GNSS remain separate.

## Two implementation findings

OTS publishes the firehose as a durable fanout exchange. In the local source,
`opentakserver/eud_handler/EudHandler.py::publish_cot()` publishes a JSON
envelope containing `uid` and `cot`. The separate parser exchange also gets
`user_id`; the firehose does not. A claimed UID is insufficient evidence that
an authorized operator issued a command. A trusted gateway must not sign every
matching firehose event and thereby make unauthenticated input trusted.

Columba exposes `RnsCore.observePackets()`, but generic destinations currently
do not feed it. The Python backend's `handlePacket()` only logs a placeholder;
the Kotlin backend declares a packet flow without emitting into it. Creating
and announcing a destination alone will not deliver tasks to an app consumer.
Both backend callback paths belong in this PR, with IPC delivery verified.

## Scope and trust boundary

1. Subscribe to OTS's firehose with bounded parsing, reconnect handling, and
   explicit selection of task candidates. Capture a real ATAK task event
   before adding automatic type or recipient mapping; do not infer addressing from
   callsigns or the position codec's four-byte sender identifier.
2. Require authorization before signing. Initially, an imported firehose task
   can be staged for explicit approval by the local command-post operator.
   Automatic forwarding requires authenticated operator provenance available
   at this boundary and an explicit authorization policy. Broker access or a
   matching UID alone does not supply that provenance.
3. Encode one bounded binary task with a full recipient destination hash,
   task identifier, issue and expiry times, coordinates, short UTF-8
   instruction, and issuer identification. Sign the canonical bytes with a
   task-specific domain separator. Reuse the identity-signing primitives,
   keeping task authority separate from permission to supply time.
4. Verify against an explicitly trusted issuer key on the phone. Reject wrong
   recipients, bad signatures, unknown versions, malformed fields, expired
   tasks and unreasonable future timestamps before persistence or display.
   Persist replay protection across restarts; a duplicate must not create a
   fresh task, notification, or acceptance decision.
5. Wire incoming SINGLE-destination packets through both Columba backends and
   IPC. Persist verified tasks per local identity, provide trust configuration
   and an inbox, and distinguish received, accepted, declined and expired.
6. Return signed, recipient-bound task status to the command post. Show transport
   failure and missing acknowledgment honestly. Bound retries and deduplicate
   both directions; never label an enqueued packet as operator acceptance.

RADs forward encrypted Reticulum packets using their existing transports.
This task does not need a firmware position source, a new radio protocol, or
an executable-command interpreter on a RAD. Firmware-originated position is
still unproven and remains a separate acceptance item for peer position or
on-board GNSS.

A signed task is deliberately larger than a 21-byte position update. Measure
the complete encrypted packet size and airtime, including signatures and
Reticulum framing, before choosing the text limit and retry policy. Keep the
task inside a single packet and publish the actual budget.

## Merge gates

- Golden wire vectors shared by Python and Kotlin, including UTF-8 boundaries.
- Tamper, wrong-key, wrong-recipient, expiry, replay-after-restart and malformed
  input tests. Unknown or unauthenticated input must never become a trusted task.
- Receiver tests for both Columba backends and the process boundary.
- Packet size and retry budget measured with the actual task envelope.
- OTS to gateway to RAD to phone, then user response back to command post,
  demonstrated on hardware with the phone screen off before task arrival.
- Disconnect and reconnect exercise: no false acceptance and no duplicate task.
- Existing position reporting remains functional throughout the exercise.

Completing this PR establishes one task workflow, not full TAK compatibility.
Shared markers, chat, outdoor failover, and radio awareness retain their own
acceptance gates in the roadmap.

## Running the implementation

`tools/tak_tasking.py` is a separate command-post service. It leaves the position
gateway and existing OTS processes running. Use the host's RNS interpreter;
firehose subscription also requires `pika` in that interpreter.

```sh
PYTHON=~/.local/share/rnode-rns-venv/bin/python
$PYTHON tools/tak_tasking.py serve --firehose
```

The default private database is `~/.impr-tak/tasks.sqlite3`; the separate task
authority key is `~/.impr-tak/task-authority`. Back up that key. Startup prints
its **public** key and acknowledgment destination. The phone trusts the public
key, not the position gateway hash. `TAK_AMQP_URL` can supply a broker URL; its
default is the local OTS RabbitMQ service. Credentials are not logged.

In Columba, open **Settings → TAK tasks**, enter the command post's 128-character
public key and save. Receiving is off with an empty key. Trust and inbox state
are isolated by the receiving identity. Copy the phone's public key from the
same card and pin it on the command post:

```sh
$PYTHON tools/tak_tasking.py peer responder PHONE_PUBLIC_KEY
$PYTHON tools/tak_tasking.py list
```

Firehose events of type `t-*` with valid coordinates become **untrusted point
candidates**. The CLI displays their original type, claimed origin, target hint
and text. It does not treat arbitrary task types as a supported go-to protocol,
nor infer a recipient. `approve` explicitly converts the selected point into a
new go-to instruction chosen by the local operator:

```sh
$PYTHON tools/tak_tasking.py approve CANDIDATE_ID --peer responder \
  --instruction 'Meet at the marked point' --lifetime 900
```

The instruction is required even when the imported CoT has remarks. There is
no automatic-signing option. An operator can also create a task directly, or
import an OTS envelope from disk for reproducible testing:

```sh
$PYTHON tools/tak_tasking.py goto 41.1234567 29.1234567 --peer responder \
  --instruction 'Bench test: review this point' --lifetime 900
$PYTHON tools/tak_tasking.py stage firehose-envelope.json
```

The phone persists verified tasks before notifying or responding. **Received**
means the phone verified and stored the task; **accepted/declined** require a
user decision. Decisions are terminal. `list` shows verified responses on the
command post; a send attempt never claims receipt. The phone retains its
decision if acknowledgments cannot get through and reports only the number of
attempts, not confirmed delivery. Expired tasks cannot be acted on.

Deleting the private phone database deliberately also deletes its replay
history. Ordinary app and backend restarts retain it. Old records are retained
past their expiry; expired signed packets are independently rejected.

## Wire and transmission budget

All integers are big endian. Every packet starts with version (1 byte), kind
(1), issuer identity hash (16), recipient task-destination hash (16), task ID
(16), issue time (4) and expiry (4). Go-to payloads append latitude and longitude
in degrees times 1e7 (4 each), a UTF-8 byte length (1), and 1–64 instruction
bytes. Status packets append one byte: received=1, accepted=2, declined=3.
Both append a 64-byte Ed25519 signature over `urtn-tak-task-v1\0` plus the
entire preceding body. Task lifetime is at most one hour; future skew at most
30 seconds. Receiver wall clocks must be correct.

Measured by packing real Python RNS SINGLE packets:

| Packet | Signed payload | Encrypted packet | With routed header | Estimated airtime |
| --- | ---: | ---: | ---: | ---: |
| Maximum go-to | 195 B | 307 B | 323 B | 245 ms |
| Status | 123 B | 227 B | 243 B | 186 ms |

Airtime uses the existing firmware model at SF7/BW250/CR4:5 and the routed
length. It is an estimate, not a radio measurement. Both fit one Reticulum
packet.

**Retries are bounded by the task's own expiry, not by an attempt count.** Both
directions retry every 60 seconds for as long as the task is valid, and a
verified receipt stops the downlink at once. The earlier three-attempt cap
spanned two minutes, which had nothing to do with how long a task mattered: a
responder who walked through a dead spot exhausted it and never heard the task
again, while the task itself stayed valid for another thirteen minutes. Path
discovery does not consume a retry in either direction -- no packet was
transmitted and none was refused.

A default 15-minute task therefore costs at most ~15 downlinks at 245 ms, about
3.7 seconds of airtime spread across those fifteen minutes, plus the responder's
receipt and its one decision on the same schedule -- and in the ordinary case
where the first downlink lands, it is one transmission each way. Excludes
announces, path discovery, and lower-layer overhead.

`tests/fixtures/task_v1.json` and Columba's matching test resource contain the
same deterministic test-key signatures, including a non-ASCII instruction.
The keys in these fixtures are test vectors, never deployment identities.

## Acceptance record — 2026-09-08

The phone leg is now proven end to end on hardware. Getting there required a
harness that could drive the inbox, and that harness immediately exposed three
defects, none of which any unit test could have caught: all three lived in the
seams between the app, the backend and the transport.

**What the phone harness added.** Four debug actions — `GET_TASK_KEY`,
`SET_TASK_AUTHORITY`, `LIST_TASKS`, `RESPOND_TASK` — driving the real
TaskManager, store and backend. Acceptance had been stalled on "somebody has to
hold the device"; the only step still needing a person is the tap itself.

### The three defects

1. **A verify-only peer could not be addressed at all.** Every acknowledgment
   failed with `RnsException: Identity not found: <authority hash>`. The Python
   backend's `resolveIdentity()` could return an identity from cache or rebuild
   one from a private key, and had no third case — so a peer known only by its
   public key, which is every remote peer and precisely what a pinned task
   authority is, could not be turned into an OUT destination. The native
   backend already fell back to `fromPublicKey()`; the two are now in line, via
   a keyless `RNS.Identity` given `load_public_key()`.

2. **Path discovery consumed a delivery attempt.** `sendStatus()` marked an
   attempt before testing for a path, so a receipt could exhaust its whole
   budget of three waiting for a route it never got to use. Discovery now
   defers without spending an attempt.

3. **One failed announce ended the receiver permanently.** The announce used
   `getOrThrow()` inside the receive loop, so a single failure threw out of it
   and the destination was never advertised again for the life of the process.
   The visible symptom was the worst kind: trusting an authority through the UI
   produced a phone that verified nothing and could not be reached, and looked
   fine until someone restarted the app. Announce failures are now logged and
   retried at 30 s until one succeeds.

### Verified on hardware

- **Accept**: task issued, verified and persisted in 5 s, accepted, and the
  command post recorded the signed acceptance — `"state": "accepted"`.
- **Decline**: same round trip, `"state": "declined"`.
- **Screen off**: with `dumpsys deviceidle get screen` reporting `false`, a task
  arrived, verified, persisted, and its signed **received** receipt reached the
  command post unprompted.
- **Untrusted issuer**: a second authority signed a well-formed task addressed
  to the same phone and delivered it. The phone refused it —
  `Rejected task: IllegalArgumentException` — and it never became an inbox
  entry, a notification or a decision.
- Unit tests green, including a new case asserting that deferring for path
  discovery leaves the attempt budget whole.

### Reconnect — 2026-09-10

The disconnect/reconnect gate failed on first attempt and took two fixes.

**Retries were bounded by an attempt count rather than by the task.** With the
recipient's only interface down, the command post spent all three attempts in
two minutes transmitting into a path that no longer terminated, and nothing
redelivered when the interface returned ninety seconds later -- while the task
stayed valid for another thirteen minutes. Both directions now retry until
expiry, which is already enforced and already capped at an hour by the codec.

**App-registered destinations did not survive an RNS restart.** With the first
fix in, the task still never arrived, and the announce hardening from 09-08 is
what made the reason legible rather than silent:

```
Task destination announce failed
RnsException: Identity not found: 5d38c5ebf72dc4bc242e3e5fe7f8705e
```

That hash is a *destination*, not an identity. `PythonRnsRuntime.stop()` clears
the destination registry -- it must, since those objects belong to a Reticulum
instance that no longer exists -- and nothing re-created the ones app code had
registered, along with their packet callbacks. LXMF never showed it because
`LXMRouter` re-registers its own delivery identity on start. The phone's
announces still reached the deck throughout, so the uplink was never the
problem: the phone simply had no registered destination left to receive on.

This is not a tasking defect. Every destination the TAK design depends on --
group destinations for teams and missions above all -- had the same exposure,
and any interface change would have ended inbound delivery silently. It is
fixed by `AppDestinationRegistry`, which retains registration intent and
replays it before `READY`, at three layers: the Python backend, the native
backend, and the UI-side proxy, so it survives the `:reticulum` process being
replaced outright and not merely restarted.

**Verified on hardware.** Interface dropped, task issued into the outage: the
phone held no row and the command post honestly reported no receipt. Interface
restored: the task arrived within about seventy seconds, **exactly once**, its
signed receipt reached the command post, and the subsequent acceptance was
verified there. No false acceptance and no duplicate.

### Measured radio path — 2026-09-10

Run over BLE and ESP-NOW with no TCP anywhere in the chain:

```
phone --BLE--> OZD-9B2DD2 --ESP-NOW--> Rev1 --UDP--> deck command post
```

**The path carried nothing at first, and the failure is worth recording** because
it was not a tasking defect and would have been misread as one. With the phone's
only interface set to BLE, a signed task never arrived; a Reticulum probe from
the deck showed 100% loss; raw packets at 8, 64 and 200 bytes were all accepted
by the sender (`receipt=True`) and none arrived, which rules out BLE
fragmentation; and the phone's own announce failed to advance the deck's path
table. Traffic was dead in both directions. Earlier path timestamps that looked
healthy turned out to predate the switch, from while the phone was still on TCP.

That is the `apply_relay_policy` behaviour described in
`fix/espnow-mesh-forwarding`: a node with no upstream of its own disabled
`Reticulum::transport_enabled()` while it held an ESP-NOW parent, so a phone
could associate over BLE, announce locally, and reach nothing. OZD-9B2DD2 has no
LoRa and no infrastructure Wi-Fi, so it has no upstream and sits exactly in that
case.

After flashing OZD-9B2DD2 with that branch, same test, nothing else changed:

| | Before | After |
| --- | --- | --- |
| Phone announce reaching the deck | path unchanged | path advanced |
| Probe, deck to phone | 100% loss | **0% loss, 2.016 s over 3 hops** |
| Signed task delivered, accepted, verified | never arrived | **complete in 75 s** |

The task landed on the command post's second attempt at about 62 seconds; the
first went out before the path had settled after the board's reboot. In steady
state the earlier TCP runs completed in a single attempt each way.

**`fix/espnow-mesh-forwarding` is therefore a hard dependency of this gate**, not
an adjacent branch, and should merge before this PR cites the result.

Flashing preserved the board. `pio run -t upload` writes bootloader, partition
table and `app0` only, and the node identity and provisioned config live in the
LittleFS partition at `0x2D0000` -- as `boards/ozdisan_esp32.csv` warns, and
which the board's continued ESP-NOW peering with Rev1 confirms. A full 4 MB
image was taken first, its LittleFS partition mounted to prove the backup
readable, and `/transport_identity` (`0056bb6a9789d02a7dd5676d4f3a0ce8`),
`/time_offset` and all three `config/ns*.msgpack` extracted -- `ns1` included,
whose loss silently empties the remote-management allow list.

### The UI card — 2026-09-10

Driven through the card itself rather than the harness, over the LoRa topology
above, in the order a real operator would:

1. Trust cleared. A signed task issued while untrusted **did not arrive** -- the
   inbox held no row for it, and the card read "Receiving is off until an
   authority is trusted". The earlier task already in the inbox was correctly
   marked "This task's authority is no longer trusted".
2. The command post's 128-character key typed into **Command-post public key**
   and saved with **Save trust setting**. The card changed to "Listening for
   verified tasks" without a restart -- which is the announce fix from 09-08
   doing its job, since setting trust on a running receiver used to kill it.
3. The task that had been refused **then arrived** on the next retry, and its
   signed receipt reached the command post. Retrying until expiry is what made
   this possible: under the old three-attempt cap the task would have been dead
   long before trust was configured.
4. **Accept** tapped in the card. It moved to "Go to point · Accepted", and the
   command post verified the signed acceptance.

The only step not performed by a person is the finger on the glass; every
component in the path -- card, TaskManager, store, backend, IPC, BLE, LoRa --
is the production one.

### A task from real ATAK — 2026-09-10

A marker dropped in ATAK-CIV 5.6.0.12 on the phone became a signed task on that
same phone, over the LoRa path, with the operator supplying the instruction:

```
ATAK marker -> OTS -> firehose -> untrusted candidate -> operator approval
            -> signed task -> Reticulum -> LoRa -> BLE -> phone -> accepted
```

The marker's point (40.9546015, 29.0922658) carried through to the task
unchanged, while the instruction — "Investigate the marked contact" — came from
the operator at approve time. The marker supplied a location; a person supplied
the order. The candidate was consumed on approval, so rebroadcasting the same
marker does not re-offer it.

**Real ATAK traffic disproved three assumptions.** None of them would have
surfaced against a synthetic publish, which is exactly why this gate existed.

*The `t-` type filter matched nothing ATAK sends.* Dropping a marker emits an
atom — `a-h-G` for a hostile, `a-n-G` for a neutral — and ATAK's marker UI never
produces a `t-` event. The filter was a guess made before anyone here had seen
ATAK on the wire. Both families are accepted now; the type was never what made
a candidate safe, the explicit approval step is.

*Auto-send makes one marker expensive.* With auto-send on, ATAK rebroadcast a
single stationary marker every ten seconds at 724 bytes. On this fleet's LoRa
that is 570 ms per transmission, **5.7% of the channel continuously, and 205
seconds of airtime an hour, for one marker that never moves.** Three of them
exceed 17% before anyone speaks. Keying candidates on the envelope hash also
meant each repeat created a new candidate, filling the 128-slot inbox in about
twenty minutes; candidates are now keyed on the marker's own UID and position,
so repeats collapse and a genuine move earns a new candidate.

*Position reports would have flooded it worse.* They are most of the firehose
and every one moves. The discriminator is in the data and held across all 42
events captured, ATAK's own PLI and this project's position gateway alike: a
self-report carries the publishing EUD's UID as the event UID, an object placed
on the map carries its own. Self-reports are skipped. Verified live — 45 seconds
of pure position traffic produced no candidates at all.

There is still no automatic signing, and no recipient is inferred: a broadcast
marker carries no `marti/dest`, so there is nothing to infer from even if that
were wanted.

### Still not accepted

Every merge gate is met. What remains is not a gate but an exercise: **LoRa at
range and under load**. Everything above crossed a bench-distance hop carrying
one task at a time. The two-Rev2 outdoor run is where airtime, range and
contention get answered, and its figures are what the published budget rests on.

### LoRa path — 2026-09-10

```
deck --UDP--> Rev2 ==LoRa==> Rev1 --BLE--> phone
```

Rev1 was rebuilt as `impr-rad01-rev1-ble-lora`: BLE added, and TCP, UDP and
ESP-NOW removed. The removals are the point. With `UDP_TRANSPORT` in place Rev1
sits one hop from the deck over Wi-Fi and Reticulum always prefers that, so the
test would pass without a packet crossing the radio. Stripped, LoRa is Rev1's
only route to anything, and the second OZD was unplugged so BLE offered no
alternative either.

**Result: 17 seconds, one attempt in each direction.** Task verified and
persisted on the phone about four seconds after issue, accepted, and the signed
acceptance verified at the command post with `attempts: 1`. Faster than the
BLE/ESP-NOW run, which took 75 seconds and two attempts.

Bytes crossed the radio in the same measurement: the deck's Rev2 interface went
743.34 KB to 744.46 KB sent and 1.04 MB to 1.05 MB received across the run.

Two findings on the way there, both about Rev1 rather than about tasking:

**BLE is not a build-time switch on a RAD board.** `HAS_BLE` is already true for
Rev1, so `BLEPeerInterface` was compiling into every image; it needed
`BLE_PEER_TRANSPORT` to be registered as a transport, Bluetooth enabled in
EEPROM (`rnodeconf --bluetooth-on`), and a power cycle. `bt_start()` is further
gated on `wireless_kiss_policy_ready && wireless_kiss_allowed`, which provisioning
only sets once loaded. Adding NimBLE is the wrong move here and fails twice
over: the core ships its own BLE stack, so every NimBLE class is redefined, and
NimBLE parameters named `MTU` collide with `Config.h`'s `#define MTU 508`.

**The BLE peer would connect and then drop** until `RRC_HUB`,
`RRC_PROTOCOL_CORE` and `LXMF_PROPAGATION_NODE` were removed from the test
build. None of them carry a task. Static RAM only fell from 35.6% to 32.5%, so
the static saving is not the explanation -- what those three do is allocate
continuously, and this board's recorded failure mode is fragmentation with flat
free memory, the largest contiguous block collapsing while totals look healthy.
A Bluedroid GATT server needs contiguous buffers, which makes it the first thing
to fail under exactly that.

**Still unmeasured: LoRa under load, and at range.** This was one task across a
bench-distance hop. The two-Rev2 outdoor exercise remains where airtime and
range get answered, and its figures are what the published budget rests on.

## Acceptance record — 2026-09-06 (superseded by the run above)

- Firmware/tool suite: **337 tests passed, no skips**, using the host RNS venv.
- Columba: **27 targeted tests passed** (18 existing position codec, four task
  codec, two durable inbox, two native packet operations, one Python packet
  publication). Python-produced signatures verify in Kotlin. Both backend
  modules and the Python debug app compile; detekt passes for the app and
  both backends. New Kotlin sources were formatted with the repo's ktlint version.
- The Python debug APK was built and installed on the A54, retaining its data.
  The new receiver started and registered its task destination. Trust remains
  unset until the task authority is explicitly configured.
- An isolated Python test endpoint connected to Rev1 TCP received and verified
  a signed task and returned a signed receipt that the command post verified.
  Its return path was initially absent. Explicit path discovery resolved it at
  two hops; the next test completed on its first downlink attempt. This test
  uses Rev1's existing firmware and its TCP/UDP forwarding, **not a measured
  LoRa or ESP-NOW path**.
- A **synthetic** `t-x-bench` event published to the actual OTS RabbitMQ firehose
  appeared only as an untrusted candidate. Explicit CLI approval produced a
  separate signed task; the Rev1-connected endpoint verified it and the command
  post recorded its signed receipt. No automatic command was produced on import.
- The A54 was locked during the remaining UI work. **Not yet accepted:** saving
  trust through the phone UI, screen-off task arrival, a human accept/decline
  response over the real phone/backend/IPC path, reconnect behavior on the phone,
  and a real ATAK-authored task capture. No claim of field readiness or completed
  phone acceptance is made by the host-side probe results.

The development service and its test-only database/key live under
`~/.impr-tak/tasking-dev/`, separate from the production position gateway.
Use the corresponding `--db` and `--identity` arguments when continuing that
acceptance run. The temporary probe identities are not field recipients.
