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

A clean 9.47-hour run on 2026-08-30 differed from the failing night in **four
ways at once**, so none of them is yet implicated:

| | Failing night | Clean 9.47 h run |
| --- | --- | --- |
| Power | Power bank | Deck USB |
| WiFi | Away from home AP | In range |
| BLE peer | Intermittent | Continuously connected |
| Motion | Moving | Stationary |

Leading hypothesis, untested: the **WiFi** one. A station repeatedly failing to
reassociate while out of range of its AP is a plausible way to starve the
watchdog, and it would make this a mobility-specific fault -- which matches the
deployment case.

**Discriminating test, single variable:** run it on the power bank *at home,
stationary, in WiFi range*. Clean means power and battery are not the trigger and
the away-from-WiFi condition is. A `TASK_WDT` means it is power-related after
all, despite the absence of brownouts.

**Next diagnostic after that:** hold a console attached and wait for a reset. The
task watchdog prints which task failed to check in, with a backtrace, immediately
before resetting. That names the culprit outright. Attaching a console resets
Rev 1 -- but that no longer matters when the reset is the event being waited for,
which is what made this measurable at last.

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
