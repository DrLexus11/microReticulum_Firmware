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

**Status: open, found 2026-09-12 on the first end-to-end hardware run. This is
the one that cost the evening.**

Everything the endpoint rendered while ATAK was disconnected went nowhere, and
nothing said so. The only evidence was a log line added while hunting it:

    -> ATAK: 637 bytes to 0 client(s)

Markers and chat were decoded and rebuilt correctly the entire time. The frames
crossed the mesh, the renderer produced valid CoT, and it was written to an
empty client list. From the operator's side that is indistinguishable from the
mesh not working, which is exactly the wrong conclusion to reach on a hillside.

**Leading hypothesis: the endpoint never answers ATAK's ping.** ATAK sends
`<event type="t-x-c-t" …>` to its server as a liveness check, and a TAK server
replies `t-x-c-t-r`. Ours ignores it, so ATAK concludes the server is dead and
cycles the connection. Observed: a connection alive at 18:01:20, gone by
18:02:30, back by 18:04:59.

Worth checking before anything else, because it is cheap to test and would
explain the whole pattern. If it is not the ping, the next candidates are the
endpoint dropping clients on a write error and ATAK's own reconnect policy.

**Why it matters more outdoors.** Indoors it costs a repeated test. In the
field every drop is a hole in the picture that nothing reports, and it will
read as a range problem during Outdoor Test 1.

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

