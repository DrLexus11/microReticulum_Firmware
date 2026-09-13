# TAK delivery plan — the phasing, and what gates what

**This file is the single source of truth for what ships in which PR.** It
supersedes the A–J sequence table that used to close `TAKNative.md`. Design
rationale and acceptance evidence stay in `TAKNative.md` and
`TAKIntegrationPivots.md`; this file answers only *what is being built next and
why in that order*.

Agreed 2026-09-12. Revise it when the order changes — do not let the order live
only in a conversation.

---

## The decision behind the order

**Build the infrastructure first. Let the plugin fill what is left, which is
only ever the things TAK's own data model has no field for.**

The rule: *the layer a gap lives in decides the fix.* Transport semantics —
delivery, retry, ordering, offline queue — belong below ATAK, where every
client benefits and ATAK cannot tell the difference. Protocol surface —
missions, data packages — is infrastructure too, as a local shim, because that
is the only route that keeps ATAK's native tools working at all. Only
information TAK has no field for is the plugin's territory.

Two options were rejected explicitly:

**A plugin that replaces chat, teams, contacts and missions.** ATAK plugins are
Android-only. The fleet has iTAK and WinTAK operators, and this abandons them on
day one. It is also the largest build for the least return: our chat problems
are transport problems, and a new interface over a lossy transport is still
lossy.

**Infrastructure with no plugin at all.** Gets roughly 80%, and it is the right
80%, but it hits a wall where honest transport produces states ATAK cannot
render. Suppress room receipts to save the channel and ATAK shows no delivery
tick — which an operator reads as failure, when the truth is *delivered to 8 of
9, 1 queued until RAD-04 is back in range*. There is no CoT element for that
sentence. **The plugin's job is to stop the interface lying, not to replace
it.**

## Scope: what LoRa carries, what waits for HaLow

**On LoRa:** positioning, markers, drawings, routing, messaging. That is the
whole requirement, and it has to be solid rather than broad.

**HaLow phase:** images, data packages at full size, then voice, then possibly
video. A 1 MB package is 13.7 minutes of LoRa — a category difference, not a
tuning problem. Field duty-cycle approval is sought separately for a limited
voice and video trial; in the lab it all runs under the existing approval,
where gain is the only constraint.

---

## The sequence

| | | Contains | Gated on |
| --- | --- | --- | --- |
| **B** | *Membership and typed codecs* | Shipped. Close as is. | — |
| **C** | *Chat that survives a partition* | **Built and proven.** Addressed chat on LXMF; room receipts suppressed at the endpoint; replay buffer for a detached ATAK; store-and-forward proven end to end 2026-09-13 | nothing |
| **D** | *Everything that does not fit one packet* | Tier 3 `Link`/`Resource` fragmentation; typed polyline codec for drawings; wire format for a blocked route edge | nothing |
| **E** | *The node knows where it is and what it can reach* | GNSS NMEA on the second UART; the relaying/boundary resolution; the ESP-NOW reset trigger; BLE proven as the endpoint's carrier | the two findings below |
| — | **Outdoor Test 1** | Range, disconnection, reconnection, with a mission executable at the far end | C + D + E |
| **F** | *The Reticulum ATAK plugin* | Delivery state, what is queued for whom, reachability and hops, fetch cost before spending it, propagation status, consent. **Lands in its own repo, not this one** — see *The second plugin repo* | Outdoor Test 1, and plugin know-how from the sibling repo |

C and D collapse into one PR cleanly if a single review is preferred. **E stays
separate regardless**, because it is not yet known whether it is a
configuration line or a fortnight.

### Why not one big PR

Items C through E are three unrelated areas touching different files, and one
of them is gated on a hardware investigation with no estimate. Bundling them
means one enormous review and a feature PR that cannot merge until a firmware
mystery is solved.

---

## What each PR closes

Gap identifiers match the standing record and `TAKNative.md`.

### PR C — chat that survives a partition

- **G1.** Chat has no delivery guarantee. The design calls tier 2 *must arrive*
  and names LXMF; the implementation is one bare `Packet(...).send()` at
  `tools/cot_bridge.py`, and the same on the Kotlin side. The typed codec made
  chat cheap and quietly moved it onto tier-1 transport. ATAK's delivery receipt
  is a *report* that something landed — it is not a retransmission mechanism,
  and ATAK never resends.
- **G4.** Nothing is queued for a peer out of range. `RRCBridge.cpp` already
  holds the primitive: a persisted per-room roster with a one-shot backfill
  flag. The CoT path does not use it. Chat frames already carry the author's
  send time, so a replayed backlog orders correctly.
- **G3.** Receipts are two-thirds of group chat. One line into a ten-person room
  costs 11.1 s of channel, of which 7.2 s is receipts carrying one bit each.
  Suppression sits where `forwardChat` already is.

Suppressing receipts creates the first state ATAK cannot render honestly. That
is not a side effect — it is PR F's opening brief, and it should be written
down as it appears.

#### What was built, 2026-09-12

A direct message goes by LXMF; a room line does not. That split is the one this
document already drew, and it is a property of broadcast rather than a
shortcut: a room has no member list to retry against and no receipt to wait
for.

**A direct message is also a real Columba message.** The frame rides upstream
LXMF's own payload pair — `FIELD_CUSTOM_TYPE` (0xFB) tags it `tak.chat.v1` and
`FIELD_CUSTOM_DATA` (0xFC) carries it — while the text travels as ordinary
content, so the same line lands in the recipient's Columba conversation and in
their ATAK. An operator who missed it in one can answer from the other. Room
traffic never becomes an LXMF message at all, so team chatter cannot bury
somebody's personal conversations.

Both flat, deliberately. The nested alternative under `FIELD_CUSTOM_META`
(0xFD) collides with the telemetry extras Columba already keeps there, and a
nested value has to be pre-shaped with backend-private helpers Columba's app
module cannot reach — so the Kotlin half could not have written the same shape.

**Proving it needed a check the round trip cannot make.** A direct message
arriving proves nothing: the bare-packet fallback delivers it too and looks
identical. `tak_team_acceptance.sh` asserts on the path taken, not just the
arrival, alongside a room receipt that must not arrive and a direct receipt
that must. Thirteen checks pass between two live bridges.

### First end-to-end run on hardware, 2026-09-12

Deck bridge to phone, Columba to ATAK, over a real link. Chat and four markers
rendered and delivered. Three faults came out of it that no bench test had
shown, all recorded in `CarriedIssues.md`:

- **ATAK's connection to the local endpoint flaps**, and everything written
  while it is down is silently lost. The rendering was correct the whole time
  and `-> ATAK: 637 bytes to 0 client(s)` was the only evidence. ATAK pings its
  server with `t-x-c-t`. Both endpoints now reply locally with `t-x-c-t-r`,
  serialize socket writes, and log disconnects and delivery loss. Regression
  tests cover the fix; the hardware stability run remains pending (issue 4).
- **A node that restarts is invisible for up to 30 minutes.** Both sides greet
  a *new* member, but a peer that rebooted is not new to anybody else, so
  nobody greets it and it can resolve no sender ids. `--announce-interval`
  shortens the window; the rule needs to change.
- **A direct message arrived headed by its recipient.** Fixed here — see below.

### The loop closed on hardware, 2026-09-12

Deck bridge to phone, Columba to ATAK, an operator typing a reply, and back.
Every hop is production code; nothing in the path is simulated.

    deck  -> [lxmf] direct message sent over LXMF (113 bytes, 39 of text)
    phone -> TAK chat arrived over LXMF, 113 bytes
    deck  <- b-t-f-d   delivery receipt
    deck  <- b-t-f-r   read receipt
    deck  <- b-t-f     the reply, 997 bytes, rendered for the local ATAK

The reply's event uid is
`GeoChat.urtn-da4d8be3….urtn-da4d8be3….5115990b-…`, which is the conversation
fix visible on the wire: the deck renders a message *from* the phone, so the
conversation is keyed on the phone. Before the fix that slot held the deck's
own uid and the reply was refused as "not a member of this team".

Both receipts came back, which is the other half of PR C's receipt policy
working as intended: a **direct** receipt is delivered because one peer is
waiting on exactly that answer, while a room receipt never leaves the endpoint
that made it.

### Two LoRa hops, 2026-09-12

The whole path, with no WiFi shortcut anywhere:

    phone --BLE--> Rev 1 --LoRa--> Rev 2 --UDP--> deck rrcd --TCP--> bridge

Forced by disabling the deck's UDP interface to Rev 1, so Rev 1 was reachable
only through Rev 2's radio. Rev 1's own configuration was left alone.

| | |
| --- | --- |
| Link RTT over the path | **2.16 s** |
| Rev 1's radio | −56 dBm RSSI, 9.5 dB SNR |
| Hops, deck to phone | 4 |
| Discovery | works |
| Position, phone → deck | works, every ~3 minutes |
| Markers, deck → phone | all four, ~1 s apart |
| Chat over LXMF | works, ~30 s end to end |

**The failure it exposed is the one worth keeping.** Before the fix below,
positions crossed from the phone to the deck while every marker and chat line
sent the other way vanished — and `could not reach` was printed **zero** times.

`Packet.send()` does not raise for a destination with no path. Knowing who
somebody is and knowing how to reach them expire on different clocks, and on a
four-hop path with a thirty-minute announce interval a member stays known long
after its path has gone. The bridge asked for a path only when it could not
recall the identity, which is the *other* failure. So it addressed a known peer
over a route it did not have, the frame left, nothing carried it, and nothing
said so.

It now checks `Transport.has_path()` before sending, asks for one when it is
missing, and says so. After that, markers and chat both crossed on the first
attempt.

### Store-and-forward, proven 2026-09-13

This section used to say the third guarantee was "implemented and unproven".
That was too generous. Standing up a propagation node showed it was **broken,
and had been since it was written** — the retry and the receipts were real, and
partition survival was a claim with nothing behind it.

**The propagation node.** `lxmd -p` on the deck, against the system daemon
(`rrcd`) rather than the bridge's private instance: the bridge sets
`share_instance = No` and owns its instance exclusively, and `rrcd` is the
transport node holding the UDP interfaces to the RADs, which is what makes the
node reachable from the mesh rather than only from the bridge. The
GROUP-destination hop problem that forced the bridge onto its own instance does
not apply — propagation runs over SINGLE destinations and Links, which are
routed. Config and its reasoning live in `~/.impr-tak/lxmd/config`.

**What was broken.** The fallback flipped `desired_method` to `PROPAGATED`,
reset `state`, and handed the same message back to the router. That cannot
work: `LXMessage.pack()` raises rather than packing a second time, and
`transient_id` — which the propagation stamp is computed over — is assigned
*only* in pack()'s `PROPAGATED` branch. A message packed for `DIRECT` therefore
has no `transient_id` and cannot acquire one while `packed` is set, so LXMF's
stamp thread calls `pack()`, hits the guard, and dies. The message store stayed
empty through six polls over two minutes while the bridge log filled with
re-pack tracebacks.

The fix is Columba's, not a new one: clear `packed`, `propagation_packed` and
`propagation_stamp`, defer the stamp, reset `delivery_attempts`, then re-aim.
`event_bridge.py` had solved this already and the firmware side had reinvented
it worse. Both halves now fail and recover identically.

**Two things found on the way.**

*The escalation was unbounded.* `handle_outbound` reports its outcome through
whatever failed callback is on the message, so leaving the direct one there
meant an unreachable propagation node produced escalate → fail → escalate,
forever — each turn regenerating a proof-of-work stamp and putting another Link
attempt on the air. A propagation node is unreachable **exactly when the mesh is
partitioned**, which is exactly when this path runs, so the loop would have
begun at the moment the channel could least afford it. Now one escalation, then
the truth.

*Store-and-forward was configured in silence.* A wrong hash, or a node with no
route, looked exactly like a working one until somebody walked out of range.
The bridge now says at startup which it has, requests a path if it does not
have one, and says plainly when it has none at all — because that is the state
that loses messages.

**The proof.** `tools/tak_partition_check.py` does it in one command, on the
bench, where a partition can be created exactly rather than by walking out of
range and hoping: a peer announces, leaves, the bridge sends to it anyway, 400 B
lands in the message store, the peer returns and collects the frame with nobody
resending anything. The node counted one message received from a client and one
served to a client. Repeatable; run it before any change to the carrier.

**Airtime notes from the same session.** LXMF's propagation stamp is a
proof-of-work with a floor of 13 that `lxmd` will not configure below —
0.59–1.70 s of CPU per escalated message on the deck, more on a handset. It
buys nothing on a closed fleet already authenticated by IFAC at the interface,
but it is upstream's floor and not ours to patch. Accepted, measured, recorded.
The node's own limits *are* ours: 8 KB per message against a default of 256,
because at SF7/BW250 a 256 KB message is minutes of continuous air — not a
message, an outage.

### The partition survived LoRa, 2026-09-13

The bench proof repeated over the real path: deck → Rev 2 (UDP) → Rev 1 (LoRa)
→ phone (BLE), three hops, with the recipient genuinely gone — Columba and its
service closed, not simulated.

```
08:27:27  sent to a peer that was no longer there
08:29:47  direct delivery gave up; escalated to the propagation node
          416 B held
          Columba reopened
          collected, store empty; node counted 3 received / 3 served
```

**Escalation took 140 s here against 60 s on the bench**, and the difference is
the finding. The path table still held a valid three-hop route to the phone —
set to expire 2026-09-20, a week out — so LXMF spent its five delivery attempts
against a route that looked healthy and was not. *A path outlives the peer it
points at.* Anything that reports "queued for whom" to an operator has to
expect a two-minute silence before the queue admits the message, and PR F must
not render that silence as success.

### The replay buffer held a message for ten minutes, 2026-09-13

The half that had never been exercised on hardware. ATAK closed, a chat line
arriving over LXMF and rendered with nothing on the socket to take it:

```
09:08:31  TAK chat arrived over LXMF, 127 bytes
09:08:31  rendered, 1038 bytes, no ATAK attached -- held
09:18:38  ATAK connected
09:18:38  Replaying 2 held event(s) to a new client
```

Ten minutes, inside the fifteen-minute window, and the operator saw the message
appear on opening ATAK with nothing re-sent from the deck. A position arriving
in the same gap was correctly not held.

**Two things the run corrected.**

*The log said "lost" for a message it had just held.* Columba printed `ATAK
delivery lost: 1038 bytes, no connected clients` for the very line it had put in
the replay buffer. Lost and held are opposite outcomes, and that sentence was
read in the one situation where which of them happened is the entire question.
The bridge already picked its wording from `keep`; this side grew the buffer and
kept the old sentence. Fixed.

*Closing Columba does not take a node off the mesh.* Android restarted the
`:reticulum` foreground service on its own — a new PID, an announce, and a path
refreshed on the deck seconds later — so a partition test built on closing the
app is not testing a partition. **Turn Bluetooth off instead:** it cuts BLE to
the Rev 1 and does not fight Android's restart policy. Worth knowing
operationally too, and not only for tests: a node an operator believes they have
shut down is still announcing.

### The full chain, 2026-09-13 — partition and replay, end to end

The sequence Outdoor Test 1 depends on, with every stage genuinely under test
and nothing simulated but the operator:

```
09:23:47  sent to a peer that was gone (BLE interface off at the handset)
09:26:47  direct delivery gave up; escalated -- 416 B held by the command post
09:31:37  node returns to the mesh
09:33:26  collected from the propagation node; rendered 1029 B
          ATAK closed -- held in the replay buffer
09:35:05  ATAK opened: "Replaying 1 held event(s) to a new client"
          on the map, with nothing re-sent from the deck
```

Three hops throughout: deck → Rev 2 (UDP) → Rev 1 (LoRa) → handset (BLE). Zero
tracebacks. **PR C's three guarantees are now all measured rather than
claimed**, and the propagation node and the replay buffer have been shown to
hand off to each other, which is the part neither proved alone.

**Escalation timing, three runs.** 60 s on the bench, 140 s over LoRa, 180 s
here. The bench peer was zero hops and failed fast; the two radio runs spent
LXMF's five delivery attempts against a stale three-hop route the path table
still rated valid a week out. *A path outlives the peer it points at,* and
anything reporting queue state to an operator has to expect minutes of silence
before the queue admits a message.

### A node that comes back does not ask what it missed

**Found here, and it is the gap that matters most for the field.**

Columba's retrieval is a timer, not an event. `startPeriodicSync` loops on
`RETRIEVAL_INTERVAL_SECONDS`, which **defaults to 3600**. On this run the single
scheduled attempt fell *inside* the outage:

```
09:28:21  sync begins -- during the partition
09:28:25  link_establishing -> failed (241)
09:28:26  "Sync error: Connection failed (manual=false)"
09:31:37  back on the mesh
          nothing. Next automatic attempt ~10:28.
```

The message only arrived because the operator pressed sync by hand.

Two things are wrong, and they compound. **Reconnection triggers nothing** —
yet it is the single most informative event a disaster node ever has, and the
one moment the command post is certain to be holding something. And **a failed
attempt costs the whole interval**, when failures correlate exactly with
outages: the attempts most likely to fail are the ones made during a partition.
An hour of holding an empty inbox while the command post holds your traffic is
the wrong default for this system.

Owed: a sync when the node becomes reachable again, and a short backoff after a
failed attempt instead of forfeiting the interval. Scheduled into PR C's
hardening rather than deferred — store-and-forward that only delivers on a
manual press is not store-and-forward.

### Two findings from the same run

**1. RNS throttles announces to one per hour by default, and it costs identity
rather than airtime.** `DEFAULT_AR_TARGET = 3600` is applied to every interface
of a transport-enabled node; after a grace of 5, everything else is dropped
silently. The bridge announces every 45 s, so `rrcd` passed five and went quiet:
zero bytes outbound on the radio across 60 s while announces arrived at 0.2 Hz.

The symptom was a chat line that crossed three hops, decoded correctly, and was
discarded with `LXMF frame did not render: Handled` — because the phone had
never heard of the sender. **A node that restarts, or a handset that toggles its
endpoint, keeps an empty registry for up to an hour, and every frame it receives
in that window is dropped for an identity it cannot name.** It is not visibly
broken while that lasts. That is CarriedIssues #5 again, caused one layer
further down, and it is exactly the kind of inherited protocol politeness the
disaster-first rule says to decide rather than accept.

Set explicitly on the deck: **30 s** on radio interfaces, **1 s** on the LAN hop
where airtime is free. An announce is 118 ms at SF7/BW250/CR4:5, so a 30 s
ceiling bounds one destination to 3.95% of channel across ten nodes — a ceiling
for a node that has gone wrong, not an operating point.

*Trap worth keeping:* `announce_rate_target = 0` does **not** mean no limit. RNS
reads 0 as unset and applies the 3600 default, so writing 0 to disable
throttling silently selects the strictest throttle available.

**2. The endpoint was sitting on ATAK's own default port.** ATAK was found
holding `0.0.0.0:8087` with a connection open from itself to itself, which
locked Columba's endpoint out permanently: it retried every five seconds for an
hour while a message that had survived a partition never reached the map.

The first reading of this was that an operator had configured an ATAK *input*
on the port. That was wrong, and worth recording as wrong: **8087 is ATAK's
default CoT input.** ATAK listens there out of the box. Nothing was
misconfigured — we chose a port that ATAK already owns, and then competed with
it. Whichever side binds first wins, the loser retries forever, and from ATAK's
side that reads as connection flapping.

Both endpoints moved to **18087**, which is clear of every ATAK default: 8087,
8089 for TLS, 6969 for multicast SA, 4242 and 8080. The gateway's TCP listener
moved with them — it had the same collision, and 8087 is OpenTAKServer's UDP
default besides.

The contract is unchanged and now stated where it can be read: **this endpoint
is the server, ATAK is the client.** ATAK gets an outgoing connection to
`127.0.0.1:18087` and nothing listening on it. Columba says so on the settings
screen when the port is taken, rather than reporting a bare `EADDRINUSE` — which
was true, and unactionable, and cost an hour.

**This was never plugin territory and could not have been.** A plugin runs
inside ATAK's process and has no veto over ATAK's own network subsystem binding
a port from its own defaults. The plugin phase adds port pressure rather than
relieving it. Transport semantics live below ATAK, and a port collision is below
ATAK.

### What is still wired but not exercised

**A room line has no delivery guarantee and PR C does not give it one.**
Reliable multicast over a partitionable mesh needs either an acknowledgement
from every member or blind repetition. Neither is in this PR, and the plan above
never claimed otherwise.

**The ATAK half of the LoRa partition run is still owed.** The message survived
the partition and reached Columba; it did not reach ATAK, because ATAK held the
endpoint's port throughout. Repeat once that provisioning rule is applied, and
confirm the replay buffer hands it over on reconnect — the two have never been
exercised together.

**A propagation node on a Rev 2 is still the right home for the field.** The
deck is the command post in the Outdoor Test 1 topology, which makes it the
correct first home; `LXMFPropagation.h` already exists in the firmware for when
the command post has no laptop.

### PR D — everything that does not fit one packet

- **G2.** There is no fragmentation anywhere in the CoT bridge; tier 3 was
  specified and never built. Anything over the 383 B MDU is refused outright,
  which is why **drawings do not cross at all** despite being on the LoRa
  requirement list.
- A typed polyline takes a drawing from 17 packets to 2 (6 311 B → ~584 B,
  5.26 s → 0.50 s, estimated from the corpus — there is no drawing fixture yet).
  It still needs fragmentation: 584 > 383.
- **The blocked-edge wire format belongs here.** Rubble reporting in the sibling
  repo is not a marker, it is an edge state change, and the format for "this
  edge is impassable" should be designed with the codecs rather than bolted on
  afterwards. See *The sibling repo* below.

Tier 3 is also the path images and data packages ride in the HaLow phase, so
this lays that foundation early rather than retrofitting it.

### PR E — the node knows where it is and what it can reach

- **GNSS.** `Position.h` already has the `GNSS` node-position kind; the NMEA
  reader on the second UART does not exist. `OnboardGNSS.md` deferred it as "not
  on the critical path for TAK" — that is no longer true. Outdoor Test 1 puts a
  stationary Rev 2 on a hill with no phone attached, which is exactly the
  unattended case the document reserved the module for.
- **The relaying boundary** and **the ESP-NOW reset trigger**, both below.
- **BLE as the endpoint's carrier.** The BLE peer protocol exists and is tested;
  it has not yet carried the CoT endpoint's traffic. The last bench run
  deliberately removed BLE to isolate the deck hop.

---

## Two findings that gate PR E

### 1. A board that is not in TNC mode does not relay

`RNode_Firmware.ino`:

```c
if (op_mode != MODE_TNC) {
  INFO("Not in TNC mode, transport will be disabled");
  reticulum.transport_enabled(false);
}
```

Such a board **accepts BLE peers, announces itself, is reachable as an endpoint,
and forwards nothing between its interfaces.** That is the reported symptom
exactly: the OZD board's panel showing two BLE peers (phone and Rev 1) while
nothing crosses to the main mesh.

The compiled default is `op_mode = MODE_HOST` (`Config.h`) while the
*provisioning* default is `MODE_TNC`. A board flashed but not fully provisioned
therefore lands in HOST and silently declines to relay, with every other
indicator looking healthy.

The BLE interface itself is not the suspect. It is a real `InterfaceImpl` with
`_IN` and `_OUT` true, and its own header records that packets crossed and
announces were relayed.

The MAC-ordering behaviour fits the same story: Rev 1 and the OZD board are both
Espressif (`0x80` prefix), a phone is `0xC0`–`0xFF`, so by
`shouldConnect = localMac < peerMac` the two boards pair with each other and the
phone always waits. The OZD board holding both is the expected shape — it simply
has to relay between them.

**Check before scheduling any work:** the NomadNet `/page/espnow.mu` page, line
`Relaying : yes/no`, on the OZD board and on Rev 1. If either reads `no`, that
is the whole explanation and it is configuration, not a defect.

### 2. ESP-NOW peer presence correlates with the watchdog resets

`CarriedIssues.md` §1 has had *cause found (TASK_WDT), trigger not isolated*
open since 2026-08-30, with resets at 240 s / 540 s / 2581 s / 11641 s —
irregular, which is what load-dependent starvation looks like.

Observed 2026-09-12: the OZD board averaged **~2 hours** with another OZD board
present as an ESP-NOW peer. With that peer unplugged it reached a day, and
**16 h 33 m** after a hand restart.

That is the first variable found that moves the reset interval. It belongs
**before** Outdoor Test 1, not inside it: a board that resets mid-test
invalidates every range and reconnection measurement taken, and there is no way
afterwards to tell which readings were poisoned.

---

## Open: the mesh and the server are two pictures

Team members arrive over Tailscale and talk to OpenTAKServer on the deck; the
mesh talks to `cot_bridge.py` on the same machine. **Nothing crosses between
them today.** Written up in `MeshAndServerInterop.md`, with the shape of the
answer: the deck is a member of both and a *policy point* between them, not a
tunnel — everything outward, an explicit allow-list inward, because a single
server-side client reports position faster than a LoRa channel can carry ten
mesh nodes.

Its hard parts are identity (a vouched `urtn-` UID per server-side operator, so
no device identifier reaches the air and no one collapses into "the deck") and
authority (the deck is where an OTS client certificate meets a fleet secret,
which makes it a trust boundary). Until that is designed: markers and reports
cross, commands do not.

**Clarified: the command post is the deck plus a Rev 2, both on the mesh, so it
already sees the field.** The Tailscale participants are spectators, which
makes the piece needed before Outdoor Test 1 a *one-directional* feed — mesh to
server, nothing inward. That is safe by construction: no identity to vouch for,
no trust boundary crossed, no echo possible, and no inbound airtime to gate.
Small, and worth doing before the test.

The two-way design stands for the day somebody at the far end needs to act
rather than watch. That one depends on PR D for fragmentation and PR E for the
deck's transit role.

## Later: an assistant in the loop

Raised 2026-09-12 and written up in `AssistantInTheLoop.md`: an operator asking
from inside chat what is around them, what a bearing is pointing at, what the
markers nearby are — and able to create markers and missions, with the model
configurable so volume can go to a cheaper one.

**Not scheduled, and deliberately last.** It is an interface onto the map and
building data rather than a foundation, so it cannot be better than that data;
built first it would sound confident and know nothing. It does, however, shape
two things already decided — chat as the transport it rides, and the
pull-for-one / push-for-the-area query API — so it is recorded now rather than
discovered later.

## The sibling repo — `urban-tak`, ATAK domain work, no Reticulum

`~/projects/urban-tak` owns the ATAK-side domain problem: urban navigation,
address lookup, rubble, and eventually building status. It has no Reticulum
dependency and **runs in parallel with PR C, starting immediately** — it is not
scheduled after this work.

It is a **public** repository. Nothing operational crosses into it: no fleet
secrets, node hashes, callsigns, IFAC passphrases, exercise coordinates or team
details, in any file or commit message.

**Why it starts now.** Nobody on this project has shipped an ATAK plugin, and PR
F is where that inexperience is most expensive: a bug there reads as "message
lost on a mesh" and costs days deciding whether it is the plugin or the radio.
The navigation plugin is the same skill on a problem with zero protocol
coupling, where a bug is "wrong route", visible instantly and debuggable at a
desk. It is also the only workstream not blocked on transport, and it carries
the longest lead time in the programme — an offline road graph and building
footprints for Istanbul is a data pipeline, and none of it depends on us.

The requirement, as stated: Istanbul, urban theatre, earthquake response.

1. **Road navigation.** Bloodhound gives straight-line bearing, which is useless
   between buildings. The one decision that matters on day one: **the router
   must accept dynamic edge penalties at runtime.** An engine that cannot makes
   item 3 unbuildable — rubble would decorate the map without changing a route.
   That constraint picks the engine, not benchmarks.
2. **Address lookup as a marker tool.** A non-meshed report names a street
   address. Command post staff pasting coordinates from a consumer map is slow
   and error-prone. Needs an offline geocoder over the same extract; Turkish
   addressing (mahalle / sokak / kapı no) is real work, not an import.
3. **Rubble reporting.** Not a marker — an edge state change the router
   consumes and the mesh replicates. **This is the one item that spans both
   repositories:** `urban-tak` owns the model and the routing response, this
   repo owns getting it to everyone, and the wire format is PR D's.

   **PR D has a dependency on that repo, not the other way round.** The stable
   edge identity — how a blocked edge is named so that two devices holding
   different builds of the road graph agree, and so a report survives a graph
   rebuild from a newer extract — is owed to this repo before the encoding is
   designed. It is recorded as a debt in `urban-tak/docs/MeshContract.md`. Ask
   for it rather than inventing one; an identifier chosen here that the router
   cannot resolve is worse than no identifier.

**Longer term:** clicking a building or road to report and query status, against
government feed data, with Reticulum as the query API over NomadNet.

The shape question worth settling before that API exists: an individual building
query is cheap — roughly 40 B out, 60 B back, about a second round trip at four
hops. "Status for every building on this block" is a different animal and wants
a pushed delta for the area. **Pull for the one building an operator tapped;
push for the neighbourhood they are standing in.**

---

## The second plugin repo — the Reticulum plugin is not `urban-tak`

Settled 2026-09-13. **There are two plugins, and they need two repositories.**
That was not previously written down: the plan gave `urban-tak` a repo and left
PR F's plugin implicitly inside this one, which is wrong on three counts.

| | `urban-tak` | PR F's plugin | this repo |
| --- | --- | --- | --- |
| Answers | *where can I walk, what is this address, what is rubble* | *did it arrive, who is it queued for, how far away are they, what will this cost* | *how does it travel* |
| Reticulum dependency | **must never acquire one** | **is the whole point** | is the implementation |
| Build | Android, ATAK SDK, plugin signing | Android, ATAK SDK, plugin signing | C++/ESP32 + Python tools |
| Ships as | signed APK, sideloaded | signed APK, sideloaded | flashed firmware |
| Public | yes | yes | yes |

**It cannot go in `urban-tak`.** That repo's defining constraint is no Reticulum
dependency, so that it stays useful to any ATAK user on any transport. A plugin
whose entire subject is Reticulum delivery state would destroy that on the first
commit, and the constraint is load-bearing — it is what makes the navigation
work publishable and reusable.

**It cannot sensibly go here.** This repo is ESP32 firmware and its Python
tools. An Android plugin here shares no toolchain, no build, no test runner and
no release cadence with anything already in it: firmware is flashed, a plugin is
a signed APK. The only thing the two share is the wire format, and a wire format
is a contract between repositories, not a reason to merge them — the same
argument already keeps Columba separate.

**Recommended name: `mesh-tak`** — parallel to `urban-tak`, says what it does,
carries no operational detail. Confirm or replace before the repo is created;
the naming call is not the agent's.

The same public-repo discipline applies as for `urban-tak`: no fleet secrets,
node hashes, callsigns, IFAC passphrases, exercise coordinates or team details,
in any file or commit message. It has a stronger reason to be careful, because
it handles identity and reachability rather than map data — **it may name the
concepts, never the fleet.**

**When it is created:** not yet. PR F is gated on Outdoor Test 1, and the
scaffold should be written against a transport whose behaviour is known rather
than one still being hardened. The decision is recorded now so that PR D and PR
E stop accreting Android-shaped work on the assumption it has somewhere to live
here.

---


## Standing constraints these plans assume

- Approved lab conditions: **no duty-cycle limiting**. Gain (21) is the only
  legal constraint. Field approval for voice and video is sought separately.
- Disaster-first: weigh re-meshing speed against airtime, and never inherit
  protocol politeness by default.
- Scope: microReticulum, its library, Columba, and the sibling ATAK repo. Other
  people's projects are not ours to fix.
- Push to `origin` (DrLexus11) only. `attermann` and `upstream` are
  push-disabled deliberately.
- Fleet secrets and IFAC passphrases are prompted on the terminal, never passed
  as command-line arguments.
