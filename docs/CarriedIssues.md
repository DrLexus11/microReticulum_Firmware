# Issues carried between branches

Faults that outlive the branch they were found on. Recorded here so they are not
rediscovered, and so a branch that happens to touch the same area knows what is
already known.

## 1. Rev 1 resets: cause identified as TASK_WDT, trigger not yet isolated

**Status: cause found 2026-08-30. The resets are task watchdog timeouts. What
starves the watchdog is still open.**

### What the resets actually are

The persisted `bootlog.txt` after an unattended battery run on the night of
2026-08-29 records the whole session:

    POWERON  prev=hw-reset     <- placed on the power bank
    TASK_WDT prev=540s
    TASK_WDT prev=2581s
    TASK_WDT prev=240s
    TASK_WDT prev=11641s
    POWERON  prev=hw-reset     <- power lost (bank cut out or was moved)

`TASK_WDT` is the ESP32 task watchdog: some task held the CPU without checking
in. That is a software stall, not a hardware or power fault.

### Two corrections to what this entry used to say

- **Not "roughly every two hours".** The intervals are 4 minutes, 43 minutes,
  9 minutes and 3 h 14 min -- irregular and load-dependent. Anyone hunting a
  two-hour period is looking for the wrong shape, and the longest clean stretch
  is longer than the figure this entry originally quoted.
- **Not brownout, and not memory exhaustion.** Zero `BROWNOUT` entries across
  all 77 bootlog records. Free internal heap at an observed reset was 53 KB,
  about 21% of the pool, where `RNS_LOW_MEMORY_REBOOT` fires at 98%.

The 20 `PANIC` entries in the same file were `BTC_TASK` stack overflows
introduced and fixed during the BLE peer work (PR #14) -- BLE callbacks calling
into Reticulum's parse and route path, which has no business running on that
stack. They are not this fault. With them gone, `TASK_WDT` is what remained
underneath.

### What is still unknown: the trigger

**Environment is ruled out.** A `TASK_WDT` was recorded on 2026-08-30 with the
board on deck USB power, at home in WiFi range, stationary, and with the BLE peer
continuously connected -- ending a 37208s (10.34 h) run. That eliminates every
environmental variable that distinguished the failing battery night:

| | Failing night | Also fails here |
| --- | --- | --- |
| Power | Power bank | Deck USB |
| WiFi | Away from home AP | In range |
| BLE peer | Intermittent | Continuously connected |
| Motion | Moving | Stationary |

An earlier revision of this entry proposed WiFi reassociation while away from the
AP as the leading hypothesis, and suggested a power-bank-at-home test to isolate
it. **That hypothesis is refuted** -- the fault occurs with none of those
conditions present. It is recorded here so it is not proposed again.

What is left is that the stall is in normal operation, independent of
environment, with a wildly variable interval: 4 minutes, 9 minutes, 43 minutes,
3 h 14 min and 10 h 20 min are all observed. That spread argues against a simple
periodic task and for something load- or state-dependent.

**Next diagnostic:** hold a console attached and wait for a reset. The ESP32 task
watchdog prints which task failed to check in, with a backtrace, immediately
before it resets. That names the culprit outright, and it is the only remaining
unknown. Attaching a console resets Rev 1 -- irrelevant here, because the reset
is the event being waited for. Budget hours, not minutes, given the interval
spread.

### A variable that moves the interval, observed 2026-09-12

The first thing found that changes how often this happens: **an ESP-NOW peer
being present.**

An OZD board averaged **~2 hours** between resets while a second OZD board was
plugged in as an ESP-NOW peer. With that peer unplugged -- done to isolate a BLE
test, not to chase this fault -- the same board reached a day, and **16 h 33 m**
and counting after a hand restart.

That is one observation and not yet a controlled A/B, so it is a lead rather
than a cause. It is a good lead because it fits the shape this entry already
established: load- or state-dependent, environment-independent, wildly variable
interval. It also points at a specific place to read code -- whatever the
ESP-NOW path does while holding a task, and both ESP-NOW and BLE are protocols
this project implemented from scratch.

**The controlled test worth running:** the same board, same power, same
position, peer plugged and unplugged for matched multi-hour windows, with a
console attached for the backtrace. If the correlation holds it converts this
entry from "trigger unknown" to "trigger named".

**A natural experiment already ran, noticed 2026-09-12.** The Rev 2 boards have
been soaking for two weeks. Rev 2-2 reports its ESP-NOW interface **down**, with
zero peers and zero discoveries, and has not reset in that time. The OZD board,
with one ESP-NOW peer plugged in, averaged about two hours.

Two weeks against two hours, on the same firmware family, with ESP-NOW the
variable that differs. That is not a controlled test -- different boards,
different loads -- but it is the same direction as the plugged/unplugged
observation and much longer. Taken together the two make ESP-NOW the leading
candidate rather than one of several.

**Read the boards' bootlogs before reflashing them.** Two weeks of clean uptime
is the evidence this entry has been missing since August, and a flash spends it.
`bootlog.txt` is persisted, but confirm it survived rather than assuming.

This is scheduled inside PR E -- see [`TAKDeliveryPlan.md`](TAKDeliveryPlan.md).
It must land **before** Outdoor Test 1: a board that resets mid-test invalidates
every range and reconnection measurement taken, with no way afterwards to tell
which readings were poisoned.

### Considered and currently disfavoured

`BLEPeerInterface::drain_inbound()` was changed during PR #14 review from a
bounded drain to draining until the queue is empty. If a peer delivered faster
than Reticulum processed, the main loop could sit there indefinitely -- exactly
how a task watchdog trips. The 9.47-hour run weakens this: 1133 inbound packets
with a continuously connected peer, zero drops, no watchdog. Not disproven, since
the failing night's load pattern differed, but it is no longer the first place to
look.

### The measurement problem, now solved

This issue stayed open for months because reading the boot banner required
attaching a console, and attaching a console resets this board
(`USB_UART_CHIP_RESET`). Uptime read 7 seconds immediately after one reattach
during earlier diagnosis, which invalidated every reset count gathered that way.

That circularity is gone. Two things fixed it, both built during the BLE work:

- **Provisioning metrics** (ns108): reset reason, reset code, boot count,
  previous uptime, heap and largest free block -- readable with DTR and RTS held
  high, which does **not** reset the board.
- **`bootlog.txt`**, persisted to flash and echoed at every boot, which survives
  power loss and therefore survives the power-cycle that reading it costs.

Note the distinction that matters in practice: the boot counter lives in RTC
memory and survives a *reset* but not a *power loss*, so a board moved between
power sources comes back reading `boots=1`. The flash bootlog is the only record
that crosses a rail loss.

### Heap decline, measured twice

There is a real, slower heap decline underneath all of this. Measured across the
clean 9.47-hour run:

    at 35s      heap=53532   largest=42996
    at 9.47h    heap=46424   largest=32756

About 7 KB consumed, and the largest contiguous block down from 43 KB to 33 KB --
fragmentation rather than plain usage. It recovers fully across restarts and has
never caused a failure, but a node intended to run for days rather than hours
will meet this trend. Worth watching; it is not what causes the resets.

### The fix that did work, so it is not confused with this

The KISS listener was closing healthy clients after 6.5 seconds of no readable
data, and Columba reconnected every 11.8 seconds -- over 300 times an hour, each
cycle churning lwIP socket state in DMA-capable internal RAM that never spills to
PSRAM. That is fixed and independently proven by connection counts (17 per 200 s,
then 1 per 240 s, then a single held connection). It removed a ~15 KB per hour
drain. It did not remove these resets, and the two should not be conflated.

This also **corrects** [`BridgeBacklog.md`](BridgeBacklog.md) §5, which says a
large share of the resets were self-inflicted by the observer. That was true of
the ones counted during console captures. It is not true in general, and the
conclusion drawn from it -- that removing the observer might remove the resets --
is wrong.

## 4. ATAK's connection to the local CoT endpoint flaps

**Status: heartbeat fix implemented 2026-09-12 in the Python bridge and
Columba endpoint; hardware confirmation pending. Found on the first end-to-end
hardware run.**

Everything the endpoint rendered while ATAK was disconnected went nowhere, and
nothing said so. The only evidence was a log line added while hunting it:

    -> ATAK: 637 bytes to 0 client(s)

Markers and chat were decoded and rebuilt correctly the entire time. The frames
crossed the mesh, the renderer produced valid CoT, and it was written to an
empty client list. From the operator's side that is indistinguishable from the
mesh not working, which is exactly the wrong conclusion to reach on a hillside.

**Leading hypothesis at discovery: the endpoint never answers ATAK's ping.** ATAK sends
`<event type="t-x-c-t" …>` to its server as a liveness check, and a TAK server
replies `t-x-c-t-r`. Ours ignored it, so ATAK could conclude the server was dead and
cycle the connection. Observed: a connection alive at 18:01:20, gone by
18:02:30, back by 18:04:59.

Worth checking before anything else, because it is cheap to test and would
explain the whole pattern. If it is not the ping, the next candidates are the
endpoint dropping clients on a write error and ATAK's own reconnect policy.

### Fix and verification

Both endpoints now answer `t-x-c-t` with a fresh `t-x-c-t-r` on the requesting
socket, before identity learning or mesh routing. Replies and mesh deliveries
serialize complete XML events on each socket. The framers also accept a
self-closing `<event .../>` heartbeat without absorbing the following event.
Connection changes, write failures and delivery with no clients are logged.

Python regression tests exercise repeated fragmented heartbeats over real
sockets, isolation between two clients, and marker delivery on the same
connections, including idle reads beyond the send timeout. All 552 Python tests
and 135 Kotlin CoT tests pass. Kotlin coverage includes reply fields and
fragmented framing. This fixes
the missing heartbeat response; it does not establish that every observed drop
had that cause.

The ARM64 debug APK was installed on the Galaxy A54 over wireless ADB on
2026-09-12. Through an ADB-forwarded connection to the phone's port 8087 (the
endpoint moved to 18087 on 2026-09-13, off ATAK's own default -- see
TAKDeliveryPlan.md), four
fragmented pings (including self-closing events) received valid pongs on the
same socket at 0, 30, 60 and 90 seconds, with no endpoint disconnect.

A subsequent live deck-to-phone test delivered a direct LXMF message and the
exact text was visible in ATAK. A spot marker sent from the deck also reached
the phone's CoT endpoint at its fresh GPS coordinates. The reverse direction is
not yet proven: ATAK displayed the operator's replies, including `astra hi`,
but an attached deck CoT listener did not observe the repeat reply. The open
phone thread was titled `COLUMBA`; reply addressing needs investigation before
calling the chat test bidirectional.

**Full ATAK acceptance still required:** restart the Python bridge with this fix, hold
ATAK connected through several heartbeat intervals, and deliver markers and chat
in both directions. Check that the connection count stays stable and that there
are no delivery-loss logs. If it still flaps, use the new disconnect/write-error
logs to distinguish peer closure from an endpoint write failure. Data arriving
while no client is connected is reported but is not queued for replay.

**Why it matters more outdoors.** Indoors it costs a repeated test. In the
field every drop is a hole in the picture that nothing reports, and it will
read as a range problem during Outdoor Test 1.

**Answered 2026-09-12.** The ping hypothesis held. The endpoint now replies
`t-x-c-t-r` on the local socket, and a reconnect after the endpoint restarts
dropped from roughly three minutes to eleven seconds. Delivery to an empty
client list is logged rather than silent, which is the half of the fix that
stops the next occurrence costing an evening. Left open until a long run
confirms it stays connected under load.

## 5. A node that restarts is invisible for up to thirty minutes

**Status: open, found 2026-09-12. A one-line rule change, but it needs the
rule agreed rather than patched.**

Both implementations announce every 30 minutes and greet back when they hear a
member that is **new to them**. That covers a node joining a running team. It
does not cover the case that actually happened: a node *restarts*, losing its
own membership registry, while everyone else still remembers it. Nobody greets
it, because it is not new to anybody, so it hears nothing until the next
scheduled announce.

A node in that state is not visibly broken. It is on the air, its own announces
go out, peers see it fine — and every frame it receives is dropped by
`resolveSenderId(...) ?: Handled`, because it cannot turn a four-byte sender id
back into a member it has never heard announce. Silent, one-directional, and it
looks exactly like a codec fault.

**The rule that fixes it:** greet on *any* member announce, rate-limited, not
only on a new one. The existing greeting floor already bounds the traffic --
a peer that greets back finds a known member and stops -- so the change is to
the trigger, not to the rate limiting.

`--announce-interval` was added to the bridge as the immediate lever: shorten
it while bringing a team up, leave it long in the field where it is airtime.
That is a workaround, not the fix.

**Fixed 2026-09-12, in PR C.** The trigger now fires on any member announce
rather than only a new one, on both implementations. The bound did not move:
the greeting floor was already there and is what keeps ten nodes powering up
together to one greeting each rather than nine. Separating trigger from floor
is the point -- the trigger is broad so a restarted node is answered, and the
floor is what keeps it cheap. The arrival log stays narrow, because it is about
arrival rather than about every announce.

## 6. A board can answer ping for hours while its radio has stopped

**Open. Found 2026-09-13, cost about three hours of confused testing.**

Rev 2 (192.168.1.88) stopped transmitting Reticulum UDP at approximately 11:12
and did not resume until it was power-cycled at 14:06. Throughout, it answered
ICMP normally.

What made it expensive is that every symptom pointed somewhere else. Traffic
still flowed **deck to handset**, because the deck's packets reached Rev 2,
crossed to Rev 1 over LoRa and reached the phone over BLE — so the phone's map
was populated and the phone's own logs showed BLE fragments arriving seconds
before the diagnosis. Only the return leg was gone. The visible effects were a
phone that could see the command post but could not be seen by it, chat that
did not work, and markers that appeared to work because they had arrived before
11:12 and were still drawn.

What settled it, in order:

```
Rev2 UDP interface, 90 s:  up 623.71 -> 625.62 KB      transmitting
                           down 368.23 -> 368.23 KB    nothing at all
path table via Rev2 UDP:   every entry expiring 11:12, none later
announce sniffer, 190 s:   4 announces, all from the deck itself
ping 192.168.1.88:         replies normally
```

The ping replies are the useful part: they arrive *from* the board *at* the
deck, which proves the deck's receive path is fine and narrows the fault to the
board's application rather than the network or the host.

**Cause unknown.** The board was not flashed or reconfigured that day. It is the
same family as issue #1 (a board that is not in TNC mode does not relay) and
issue #2 (Rev 1's TASK_WDT resets) in that the failure is silent, but neither
explains a board that keeps its IP stack up while its radio side stops.

**What was done about it.** Not a fix — a detector. `cot_bridge.py` now prints
when nothing has arrived from the mesh for fifteen minutes while it knows
members, and prints again when traffic resumes. It restarts nothing and times
nothing out; it only ends the situation where the command post reports itself
healthy while talking to nobody. A node that has never heard anyone is left
alone deliberately: that is a node that is alone, not one that is deaf, and the
two want different actions from an operator.

**Still owed:** the same detector on the Columba side, and some way for a board
to notice this about itself. A deck that can say "my radio went deaf" is better
than one that cannot; a radio that can say it is better still.

## 7. Links stopped establishing for most of a day, then started again

**Cause not established. Found and lost again 2026-09-13.**

This entry was first written as "links do not establish across the BLE path".
**That was wrong**, and it is left corrected here rather than deleted, because
the reasoning was sound and the conclusion still did not hold -- which is worth
more as a record than a tidy story.

### What was observed

Everything that failed needed a **Link**; everything that worked was a single
**packet**:

```
positions   packet   reliable
markers     packet   extremely reliable
chat        Link     never arrived
prop sync   Link     "Sync error: Connection failed", repeatedly
```

A link dialled from the deck to the handset's inbox resolved a path in 1.0 s,
reported 3 hops, and **CLOSED after 24 s** -- RNS abandoning establishment --
while 21-byte position packets crossed the same path without a miss. Moving
Columba to TCP drained a ten-message backlog in **170 ms**.

### Why the BLE conclusion was wrong

The TCP move changed two things at once, transport *and* hop count, and that was
recorded as a caveat at the time. The controlled test was to be Rev 1's UDP
interface, shortening the path to two hops with BLE still in it.

Before that test could run, the same link was dialled again and came up:

```
path known, hops: 3
link ACTIVE after 3.8s
  RTT 3.68s
```

Three hops, BLE in the path, establishing in under four seconds. Chat and
markers were working both ways at that moment. **So neither BLE nor the hop
count was the cause**, and the evidence that pointed at them was a coincidence
of timing.

### What actually correlates

The failure window ran from roughly 11:12 to about 14:42, and it overlaps
issue #6 almost exactly -- Rev 2 silently ceasing to transmit at 11:12. But it
did **not** end when Rev 2 was power-cycled at 14:06: links were still failing
at 14:26. What it did end with, within minutes, was `rrcd` being restarted at
about 14:42.

That suggests a transport node can carry bad state across an interface outage --
state that still passes packets while preventing link establishment, and that
outlives the recovery of the interface that caused it. **Suggests, not shows.**
Rev 1's UDP interface was enabled in the same restart, so two things changed
together for the second time in one investigation.

### What to do next time

Do not conclude from a change that moves more than one variable, however
strongly the result points. Both times that rule was broken here it produced a
confident wrong answer.

The cheap diagnostic is now known and takes seconds: dial a Link to a peer's
LXMF inbox and time it. It separates "packets cross but links do not" from
everything else immediately, and it should be a tool in `tools/` rather than a
thing retyped under pressure.

**Still open:** whether `rrcd` really needs a restart after an interface outage,
and if so what state is stale. Reproducing it means reproducing issue #6, which
is itself not understood.

## 8. The slow path is crowded, and much of the crowd is ours

**Open. Baseline taken 2026-09-13 on the LoRa path, deliberately the slowest
one available.**

Links on `phone -> BLE -> Rev 1 -> LoRa -> Rev 2 -> UDP -> deck` do establish,
but unreliably and with a six-fold spread in latency:

```
15 attempts, 3 hops:   1.4s 1.4s 1.4s 1.2s 5.4s then ten consecutive failures
after 90s rest, 25s apart:  0 of 6
one attempt, minutes later: ACTIVE in 6.8s, RTT 6.70s
best observed RTT 1.14s, worst 6.70s
```

That shape is not random loss. It is bursts of success separated by runs of
failure, which is what a **crowded half-duplex channel** does to a
latency-sensitive handshake: when the air is quiet a link comes up in 1.2 s, and
when it is not, three round trips cannot all find a gap before RNS gives up.

**Measured occupancy: 5.21 KB across the LoRa path in 60 s, about 7.1% of wall
clock** by `tools/position_budget.py` — whose own verdict for that figure is
*"crowded -- expect losses on a half-duplex channel"*.

### Much of that traffic is ours, and some of it was added today

- **The bridge announces every 45 s**, which was chosen for bench convenience
  and never revisited for a radio.
- **Each announce is now two**, node plus LXMF inbox, since the fix for PR C
  review note 7. That doubled announce airtime, and the greeting path amplifies
  it further: six inbox announces were observed in 90 s, roughly one every 15 s.
- **The handset asks the propagation node for held messages whenever a peer is
  heard**, added the same day. Each ask is a link handshake; on this path they
  were timing out at the full 300 s watchdog and being retried about once a
  minute.

So a fix for chat latency became a generator of the congestion that makes chat
slow. **The retry is not wrong; its cadence was chosen without reference to what
the channel costs.**

### What this changes

A link handshake is roughly three round trips, and at three hops every packet is
relayed — about 96 ms of airtime each at SF7/BW250/CR4:5. One link attempt is
therefore most of a second of channel time before any message moves. Anything
that dials links on a timer needs to be priced that way.

Owed, in order:

1. An announce interval chosen for LoRa rather than for a bench, and an inbox
   announce that is *not* tied to the node's cadence — a path lasts a week and
   does not need re-announcing every 15 s.
2. Retrieval that backs off when the path is slow, instead of retrying into
   congestion at a fixed rate.
3. Re-run `tools/tak_link_soak.py` after each change. The number to move is the
   establishment rate, and it is now measurable rather than argued about.

### Result, same afternoon

All three were done and the soak re-run on the same path, same hops, same
radios:

```
                  before        after
established       5/15  (33%)   14/15  (93%)
median establish  1.4s          1.4s
worst establish   5.4s          3.7s
expected delay    ~20s          ~1s
channel traffic   5.21 KB/min   1.27 KB/min
announce airtime  0.27%         0.05%
```

**The median link was never slow -- 1.4 s before and after.** What changed is
how often a handshake found a clear channel. That is the signature of
congestion rather than of distance or a bad radio, and it is what turns the
diagnosis from a correlation into an explanation.

The congestion was ours. Two of the three sources were added the same day, both
correct fixes for real problems, both sized without asking what the channel
costs. **On this transport, when you retry is a design decision with an airtime
price, not an implementation detail.**

With the handset's retrieval backoff deployed as well, over thirty attempts:

```
                  baseline      deck only     both halves
established       5/15  (33%)   14/15 (93%)   30/30 (100%)
median establish  1.4s          1.4s          1.5s
worst establish   5.4s          3.7s          2.9s
rtt median        1.33s         1.30s         1.40s
```

**The worst case fell every round while the median did not move at all.** A
congested channel does not make a handshake slower; it makes a handshake fail to
find a gap. So the tail is the whole signal, and watching it collapse from 5.4 s
to 2.9 s is what distinguishes this from a radio that was simply weak.

Still worth saying: thirty consecutive successes is a good sample on a quiet
bench and says nothing about a channel with ten nodes on it. The figure to carry
forward is the *method* -- measure the tail, not the median -- rather than
"100%".


## 9. Phone-to-deck went silent for ten minutes after a reinstall, 2026-09-21

**Open. Cause not established; the evidence rotated out before it was read.**

Columba was reinstalled on the handset at 09:28 and its BLE link to Rev 1 came
back up at 09:28:49. From then until about 09:38 nothing the phone sent reached
the deck, and it recovered without anyone touching anything.

What is established, from what was captured at the time:

- **One direction only.** The phone put announces on BLE at 09:28:55, 09:29:03
  and 09:31:22; the deck's daemon last heard it at 09:28:04 and not again until
  09:39:10. Deck-to-phone still worked at 09:33:59 -- Rev 1 was receiving on
  LoRa and sending on BLE throughout.
- **Not announce rate limiting.** The deck daemon carries a 30 s ceiling, and
  microReticulum applies no limit at all unless an interface sets a target,
  which this firmware never does.
- **The BLE link came up oddly.** Two connections to the same board within a
  second, the first negotiating MTU 20 and the second MTU 509, and every packet
  from the phone was then sent twice.

The leading suspect is Rev 1's BLE *receive* path after that double connection,
with a stall that cleared on some timer. That is a suspect, not a finding.

**Why there is no finding.** Columba's BLE debug logging fills the handset's
app log buffer in about twenty minutes, and the window had rotated out by the
time it was looked for. A host-side capture of the handset's radio-stack log now
runs during bench sessions, so a recurrence will be readable. The next thing to
check on a recurrence is whether Rev 1 logs anything received over BLE from the
phone at all.

## 10. TAK files over HaLow: reasoned, not measured -- pick up in the HaLow phase

Carried from PR D2, 2026-09-23, under the interface-completeness rule in
`CLAUDE.md`. File transfer was proven on LoRa (preview only, by design) and on
Wi-Fi/TCP (3 MB in 33 s), and is being measured on BLE. HaLow has not been
tried at all, because no Vox hardware is on the bench yet.

What is expected, and why it still needs a run:

- **Reticulum over HaLow is IP** (UDP, Auto or TCP interfaces), so D2 sees it
  as it sees Wi-Fi: short round trips, and the measured-rate gate should fetch.
- **At range a HaLow mesh drops to low rates** -- 1 MHz channels at BPSK can
  be a few hundred kbit/s, shared across hops. The rate gate should then
  defer a large file rather than tie up the mesh; that is the case to watch.
- **The round-trip pre-filter (under 0.5 s means "not LoRa")** has not been
  checked against a congested multi-hop HaLow path. If HaLow at range
  routinely exceeds it, files will wait that could have gone.
- **Declared bitrates are guesses** (UDP/Auto claim 10 Mbit/s) and must not be
  trusted over the measurement.

To do in the HaLow phase: send a data package and a full-size QuickPic across
one Vox hop and across two at range, and record the rate measured, whether it
fetched or deferred, and how long the transfer took. Until then a
rate-limited IP link on the deck is only a stand-in, not a proof.
