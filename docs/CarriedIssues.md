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

### Rev 2-2's bootlog, read 2026-09-27, and an overnight soak

Rev 2-2 was reflashed for PR E step 2 (multi-peer BLE) before its bootlog was
read -- against the note above -- but its whole filesystem had been backed up
first (`~/.impr-tak/backups/rev2-2-20260927-1842`), and the log came out of it
with littlefs-python (PlatformIO's `mklittlefs` could not mount the image):

    boot reason=SW (ESP.restart) prev=89063s
    boot reason=SW (ESP.restart) prev=102034s
    boot reason=SW (ESP.restart) prev=90147s
    boot reason=POWERON prev=hw-reset          <- plugged into the deck, 18:38
    boot reason=UNKNOWN prev=180s              <- esptool flash_id
    boot reason=UNKNOWN prev=0s                <- esptool read_flash

**No TASK_WDT, and ESP-NOW down throughout** -- consistent with the lead above.
But **a software restart roughly every 25-28 hours.** The only ESP.restart in
this firmware is `hard_reset()`, reached through RNS_LOW_MEMORY_REBOOT at
<=2 % free heap, so this reads as a slow leak on the in-service Rev 2 build,
not a watchdog. It matters as much for Outdoor Test 1: a board restarting once
a day loses its paths each time.

Now soaking overnight with `tools/serial_soak.py` (serial, attached without a
reset, reattaching after each reboot): every boot and its reason, backtraces,
`[mem]` internal heap and largest block each minute, ESP-NOW state. At start:
internal 55 284 B free, largest block 23 540 B, ESP-NOW `strict ch=0 peers=0`
-- no peer, so this run is the no-peer half of the A/B, now on the multi-peer
BLE build with two phones attached. `--summary` gives the heap slope per hour.

**First two hours (20:38-22:41), measured:** internal heap 55 276 -> 39 324 B,
**-7.8 KB/h**, largest block 23 540 -> 20 468 B (low 16 372). No boots. Two
phones attached over BLE the whole time. At that rate internal heap is gone by
about 03:30-04:00 -- far sooner than the 25-28 h the old bootlog implies (about
2 KB/h from ~55 KB), so either this build leaks faster (NimBLE multi-peer, two
phones' traffic) or the rate is not linear. The host sleeps overnight and the
serial log with it; the board keeps running, and **what happened at the heap's
end is recoverable from its bootlog** (boot reason and previous uptime per
boot) and the soak's reattach in the morning. Read both before anything else
touches this board.

**Overnight result, read 2026-09-28.** The host did not sleep; the log is
complete, one `[mem]` sample a minute. The board **restarted at 05:29 by
RNS_LOW_MEMORY_REBOOT** (`SW (ESP.restart)`, previous uptime 37 960 s, 10.5 h),
not by the watchdog. The curve:

    20:38  internal 55 276  largest 23 540
    22:38           39 108          20 468
    01:39           20 876           7 668
    04:09           24 296           3 060
    05:09           21 504           3 060     min largest block 1 268
    05:39  (after restart) 63 124   45 044

Free internal heap fell in ~8 KB steps (21:08-21:38, 22:08-22:38, 00:39-01:09,
01:09-01:39), then held near 21-24 KB -- and the **largest free block
collapsed** from 23 KB to about 1-3 KB. It restarted with ~21 KB free in
total: fragmentation, not exhaustion, ended it. Both phones stayed attached
from 20:38 to 05:22 with no reconnects, so BLE connection churn is not the
step. The in-service Rev 2's older bootlog (restarts every 25-28 h) says the
same thing more slowly.

**Next:** the firmware now prints a `[tables]` line beside `[mem]` each minute
-- every Transport table's size -- and `serial_soak.py --summary` reports each
table's growth. Rev 2-2 is reflashed with it and soaking 24 h from 2026-09-28
morning. The table that grows in steps names the leak.

**The tables are not it** (1.7 h, 2026-09-28 morning): every Transport table
flat -- 27 new paths, 50 hashes, the rest near zero -- while the largest block
fell 47 -> 33 KB. **Logging is half of it** (A/B, runtime level NOTICE vs
TRACE, 1.5 h each): total free heap -5.8 -> -2.6 KB/h; the largest block still
fell 45 -> 37 KB, and **allocated heap blocks climbed 1 859 -> 1 927** (~45/h,
peak 1 996) -- small objects allocated and kept, scattered through the heap.
NOTICE stays for field builds regardless. **Next A/B:** the same build without
ESP-NOW (`impr-rad01-rev2-n16r2-ble-peers-noespnow`), whose send path fails
all day on this board with no peer; then the BLE peers and the LXMF
propagation node, one at a time.

**ESP-NOW is not it either** (the no-ESP-NOW build stepped the same way).

### Found, 2026-09-28: links that were never freed -- two library gaps

A diagnostics build (`impr-rad01-rev2-n16r2-ble-peers-noespnow-diaglib`, removed
2026-09-29 with the no-ESP-NOW one once the leak closed;
library branch `diag/instance-counters`) prints live instance counts of
links, request receipts, resources and packets beside `[tables]`. Compared
against the Transport's active-link count, the step had a name at once: **one
link more than Transport knew of**, with a receipt, a resource and four
packets, alive for hours after its teardown -- ~8 KB of internal heap,
exactly the step size. Inbound links (a deck opening links to the board's
NomadNet node) freed cleanly; the kept one was the board's *outbound* LXMF
peer sync, whose `/offer` is never answered -- a known, separate problem
still open.

Two gaps in microReticulum, both Python features never ported:

1. **A request never answered kept its link for ever.** The receipt holds the
   link and the link holds the receipt: a `shared_ptr` cycle nothing broke,
   because Python's response-timeout job was never ported. Fix: `link_closed()`
   fails every pending request (`99abe11`).
2. **A link whose peer vanished never closed.** The per-link watchdog --
   establishment timeout, keepalive, stale close -- sat in a TODO comment. A
   link closed only on a LINKCLOSE or a local teardown, so a peer out of
   range, rebooted, or whose LINKCLOSE was lost on LoRa, left its link here
   indefinitely; one lost request on the bench did exactly that. Fix: the
   watchdog ported as a cooperative pass on Transport's one-second link check,
   with the RTT-adaptive keepalive it depends on (`86ca5c0`, host-tested in
   `test/test_link_watchdog`).

**Measured on Rev 2-2:** 16 deck links via `tools/link_churn.py` (answered,
abandoned mid-request, left open) -- link objects equal to Transport's
active links throughout, no receipts or resources kept. On the unfixed build
three links whose closes were lost were still held 28 min later; on the
watchdog build a link whose holder was killed with SIGKILL closed at 12-13
min, as the 360 s keepalive predicts (stale at 720 s, plus grace), while a
client polling `/get` over one link kept it open for the whole run.

The leaking path itself, on the fixed build: the board's own sync to
`f11d25e5`, `/offer` unanswered, timed out after 3 min (19:30:55); by the
next minute its link and receipt were gone and internal heap back from
64 048 to 68 964 B. The same sequence kept ~8 KB for good before. (The
first outbound `/offer` comes about an hour after boot: a new peer waits a
full sync interval, and the first due attempt only computes the peering
key.)

**Overnight, closed 2026-09-29.** The watchdog build ran 13 h 47 min with
no restart (bootlog `prev=49625s`, read from flash at the next flash; the
unfixed build restarted by RNS_LOW_MEMORY_REBOOT at 10.5 h). Internal heap
68 876 B at 21:11, 67 792 B at 07:55; largest block 42 996 -> 45 044 B:
flat, where the old build lost ~8 KB per unanswered sync.

Merged to the library's master as DrLexus11/microReticulum#6 (`b3f25c9`),
after one Copilot review round: a failed callback that closed the link
again failed the remaining requests twice (6 callbacks for 3 receipts);
the set is now detached first (`9900724`, host-tested). The firmware pins
`9900724`. The ESP-NOW transport fix it sits on (`ca00ad3`) merged as #5.

### Rev 1, 2026-09-29: still TASK_WDT; reflashed with the multi-peer build

Rev 1's bootlog, read from a backup before it was reflashed
(`~/.impr-tak/backups/rev1-20260929-1142`, 73 lines): the last eight software
restarts are all **TASK_WDT**, after 56 min to 3.9 days of uptime, then five
power-ons and a **PANIC (exception) 72 s after the last power-on** -- the host
restart that morning. The leak fixed above is a heap problem, not a watchdog
one, so this is still open.

Reflashed with `impr-rad01-rev1-ble-peers` (NimBLE, up to seven phones, the
library with the link fixes; RRC hub, propagation node and TCP/ESP-NOW/UDP
left out as in `-ble-lora`). It came up relaying LoRa with 2/7 BLE peers,
one of them Rev 2-2 board-to-board, and is soaking with `serial_soak.py`.
Seen once at boot, not yet explained: `esp_littlefs: Failed to unlink path
"./hashlist_store/seg1.dat". Has open FD.`

### Overnight 2026-09-29/30: no restarts; two brownouts at a host reboot

Both boards ran the night without a restart: Rev 1 on the multi-peer build
(heap flat at 171 KB), Rev 2-2 on the fixed library (heap flat near 70 KB,
nothing orphaned). The host slept 21:51-08:13 and the soak logs with it, so
those 10.4 h are known from other evidence: Rev 2-2's bootlog (read from flash
2026-09-30) ends on the boot it is still running, and neither logger had to
reattach after the host woke, which a restart's USB re-enumeration forces.

**Rev 2-2 browned out twice** on 2026-09-29 at ~10:52 (`BROWNOUT prev=1380s`,
then `prev=0s`), after ten power-ons. All of it falls in a disturbance the
operator reported: the deck's USB hub misbehaving for ~15 min, the deck asleep
~30 min, a restart, and much plugging and unplugging -- settled by 11:30. Not
a finding against the board. (The E2 over-current item still measures supply
current under TX: `0x38` lets the PA draw up to 140 mA.)

**From 11:30, the settled picture.** Rev 1: no restart, internal heap 170.6-171.3
KB throughout. Rev 2-2: no restart; 74.6 KB at 11:30, ~4 KB less once Rev 1's
BLE link to it came up, then flat at 68-70 KB for 21 h. Dips every 30 min (to
~55 KB, largest block to ~35 KB) line up with the LXMF sync cadence and recover
within a minute. No link object outlived its link: the one minute where
objects exceeded active links (21:50) was an outbound link still pending, active
with its request a minute later.

**The soak logger misses restarts on USB-CDC boards**: they print their boot
lines before the host reattaches, so `serial_soak.py --summary` reported
"boots: 0" across these. A reattach after the first attach is the tell; the
bootlog is the record.

### The OZD ran out of memory, 2026-09-30 -- fixed

In PR E2's chain test the OZD (ESP32, no PSRAM) failed allocations under
ordinary load, three causes deep:

1. **Board-to-board BLE churn.** Its link to Rev 1, at the edge of range,
   dropped and rebuilt every few minutes; heap 27 -> 18 KB in 15 min. Fixed by
   builds that keep BLE off the other boards.
2. **The file-backed packet-hash store.** Once a minute, "BLEPeerInterface::
   handle_incoming: out of memory" in the same second as "Failed to unlink
   ./hashlist_store/journal.dat": the journal could not be removed while open
   (microStore), so each rotation met a larger file. 4 KB segments delayed it
   to minute 11. Fixed by keeping the list in RAM (`RNS_PERSIST_HASHLIST=0`),
   which did not compile in the library until microReticulum#7. Rev 1 printed
   the same unlink failure at boot; PSRAM absorbs it there.
3. **Seven NimBLE connections reserved.** Even then, announce validation and
   clock writes failed with bad_alloc and a link to the board never became
   active. Two connections freed ~8 KB (39.8 KB free after boot, 31.7 before).

After all three: ~35 KB free with a phone attached, the RNS pool reporting no
allocation faults, and a link across the whole chain to the board.

### The OZD's real headroom, 2026-10-01: ~26 KB, and it aborts at zero

The `[mem]` line's `internal` counts MALLOC_CAP_INTERNAL, which on the ESP32
includes instruction RAM added to the heap -- 32-bit access only, unusable by
malloc(). Now also `heap8`/`largest8` (MALLOC_CAP_8BIT). Just after boot the OZD
reads internal 39.3 KB but heap8 26.2 KB: the "30-35 KB free" of 2026-09-30 was
~17-22 KB, and a largest block pinned at 12276 was that instruction RAM.

It aborted twice on 2026-10-01 (07:09 after 9.9 h, 08:48 after 28 min) with
the same signature: an allocation failed with the pool and the system heap both
exhausted, then allocating the bad_alloc exception object itself failed
(`__cxa_allocate_exception` -> `__terminate`). No catch can help there. The
library now keeps the jobs lock from sticking and catches send failures
(microReticulum#8), which removes the way it used to slide into that state, but
not the base cost: Wi-Fi driver (needed for ESP-NOW), NimBLE, Reticulum and its
18 KB pool in ~26 KB. It recovers by itself -- re-pairs with Rev 1 within ~5 s.

**Treated as a fixture limit, not a product one**: every product board has
PSRAM. An abort reads PANIC with this backtrace, so it cannot be mistaken for
the TASK_WDT the E2 soak is looking for. A memory budget pass on the OZD (Wi-Fi
driver buffers, NimBLE msys) is possible and not scheduled.

### Rev 1 with a live ESP-NOW peer, 2026-10-01/02: no restart in 39.9 h

PR E2's soak. Rev 1 on `impr-rad01-rev1-espnow` (LoRa + ESP-NOW), the OZD
pinned to it as its ESP-NOW parent, both carrying the chain's traffic. Rev 1's
`boot` page over the mesh, every 30 min: one boot from 2026-09-30 18:02 to
past 39.9 h, crash and panic totals unchanged (9/8, lifetime). With the 23 h
no-ESP-NOW baseline before it, a live ESP-NOW peer is not Rev 1's watchdog
trigger on the current firmware. Its old TASK_WDT restarts (0.9-93 h) predate
the library's link watchdog and lock fixes; whether those removed the cause or
only the conditions is not proven.

### Found, 2026-10-03: relayed links over TCP that were never culled -- fixed

The spare (Rev 2, 20:6E), on F3b's soak from 2026-10-02 13:19: no restart in
19 h, but internal heap fell from 86 KB to 51 KB between 21:00 and 08:40.
`[tables]` named the table. `links` (Transport's link table, the links the
board relays) held at 0-4 all day, then rose by one every ~8 minutes from 21:00:
12 by 21:46, then 91-96 the next morning. The rate was the same before, during
and after the deck's overnight suspend (21:55-08:00), so the sleeping lxmd was
not the cause.

**Cause:** `Transport::extra_link_proof_timeout()` divided by the receiving
interface's bitrate. `TCPServerInterface` declares none (0, the library
default), so a relayed link request arriving over TCP got a proof timeout of
+inf. If its proof never passed back through the board, the entry was never
culled. Python divides too, but every Python interface declares a bitrate.

- **Not identified:** the requester, about one every 8 minutes. The board does
  not log link requests at soak level.
- **The likely shape:** a phone reaching the deck's lxmd through the board while
  the proof returns by another route.
- **The fix does not depend on who it was.**

**Fixed** in the library (DrLexus11/microReticulum#9, pinned at `676982d`).
A zero bitrate now adds no extra proof time, as the library's other bitrate
paths already treated it. A host test fails without the fix. The firmware's TCP
server still declares no bitrate, on purpose: declared bitrates are guesses
(CLAUDE.md, interface completeness), and nothing should key on one.

**Next:** the soak restarts on the fixed build. The link table should stay
level overnight and the heap should stop falling.

### Found, 2026-10-03: a panic when a held announce was reinserted -- fixed

Thirty minutes into the restarted soak (09:31), the spare panicked and
restarted itself. The decoded backtrace: `std::_Rb_tree_increment`, while
iterating `_announce_table` in `Transport::jobs()` (Transport.cpp:616). The
last line before the panic was "Reinserting held announce into table".

**Cause:** inside the loop over the announce table, a held announce (one
stashed while a path request was served) was put back by erasing and
reinserting the entry being iterated, followed by a cull of the table. The
loop's next step started from a freed node. Python assigns the value in place
and leaves the keys alone; `AnnounceEntry`'s const members rule that out in
C++.

**Fixed** in the library (DrLexus11/microReticulum#10, pinned at `17701cd`):
the reinsertions are applied after the loop, with one cull. No host test: the
path needs an announce and a path request for the same destination inside the
retransmission window, which the suites have no fixture for. The soak on the
spare is the proof.

**Noticed, not changed:** where an announce is held for a path response, the
new entry is `insert`ed, which never overwrites in a `std::map`. Python's
assignment does, so the path response may not replace the pending announce.
That gets its own PR. (It did: DrLexus11/microReticulum#11, merged
2026-10-08 and pinned with #12 below.)

### Found, 2026-10-08: a panic when a cached packet was replayed -- fixed

The 48-hour soak of `dd5cae7` on the spare reached 45.0 h, then the board
panicked three times on 2026-10-05 (11:46, 13:36, 21:06) and ran clean for
58.8 h after. Rev 1 panicked once the same morning (10:16). The serial capture
had stopped by then; board telemetry showed the restarts.

The boards keep a core dump: `rad01_8mb.csv` has a `coredump` partition and
the framework writes an ELF dump on every panic. Read the last one without
erasing anything, then decode it against the **exact** build -- rebuilt at
the same path, since asserts embed `__FILE__` and the dump checks the image's
SHA-256:

```
esptool.py --chip esp32s3 --port <by-id> read_flash 0x5F0000 0x10000 core.bin
esp-coredump --chip esp32s3 info_corefile --core core.bin --core-format raw \
  --gdb xtensa-esp32s3-elf-gdb <build>.elf
```

`LoadProhibited` at `0x9c`, in `Transport::interface_to_shared_instance()`,
called from `inbound()`, called from `cache_request_packet()`: a peer's cache
request made the node replay a cached packet with its receiving interface --
which the C++ cache does not store, so the handle was empty and its `assert`
is compiled out. Python restores the interface by name and its helpers use
`hasattr()`.

**Fixed** in the library (DrLexus11/microReticulum#12, with #11, pinned at
`9e83d72`): the interface helpers and `from_local_client()` treat an empty
interface as none, and an announce without a receiving interface is not
processed (Python drops it), so it never becomes a path with no next hop. A
host test reproduces the abort without the fix. The soak restarts on this pin.

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

That tool is now `tools/link_churn.py` (2026-09-28).

**Still open:** whether `rrcd` really needs a restart after an interface outage,
and if so what state is stale. One candidate, unproven: until 2026-09-28 the
boards never expired a link -- not a stale one, not a request that was never
answered (#1, *links that were never freed*). Reproducing it means reproducing issue #6, which
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

## 11. BLE phone-to-phone does not hold a link long enough to carry a file offer

Found 2026-09-24 running D2's BLE row of the interface matrix: NEXUS (Nexus
6P) sent LEXUS (A54) a QuickPic offer -- 694 bytes, three LXMF messages --
over a direct phone-to-phone BLE link. Nothing arrived. Both phones' logs:

- **Each phone sees the other under a new address every few minutes.** The
  A54 saw the Nexus under at least four random private addresses in twenty
  minutes; the Nexus saw the A54 under at least six. Each looks like a new
  peer, so Columba tears one `BLEPeerInterface` down and builds another --
  links lasted about one to three minutes (A8:54:C2 17:05-17:08, 86:0E:BE
  17:08-17:10, E6:1B:6B 17:10-17:12, ...).
- **Reticulum's paths die with the interface they were learned on.** The
  Nexus had heard the A54's TAK node announce, but never held a path to its
  LXMF inbox: every attempt was "pathless", its path requests went unanswered,
  and the three messages exhausted their tries in 32 s.
- **Links start at MTU 20** and only sometimes reach 512 before they drop.
- **46 GATT 133 errors** on the Nexus in the same window.
- Separately, **the Nexus's TCP interface to the deck was torn down at
  17:04:05** when its interfaces were changed, and did not return -- so the
  fallback to the propagation node had no path either. To confirm whether it
  was switched off or failed to come back.

The phone-to-board link does not show this: Rev 1 keeps a stable address,
and the A54's link to RNode 1114 carried 51 packets through the same window.

**D2 is not what failed.** The offer, the rate gate and the parts never ran:
the link underneath could not carry three small messages. The fix belongs to
Columba's BLE layer -- identify a peer by its Reticulum identity rather than
its address, keep the peer interface (and so its paths) across an address
change, raise the MTU before use -- which is PR E's "BLE proven as the
endpoint's carrier". Until then, BLE phone-to-phone is marked **not usable**
for TAK files in the D2 matrix, and the timed-parts gate stays untested there.

### Root causes, found and fixed 2026-09-26 (Columba)

Measured with the A54 and the Nexus, both on the fixes as they landed:

1. **The parent keeps a departed peer's interface two seconds.** A drop longer
   than that destroyed the interface and every path learned on it. Now 120 s,
   with the identity cache kept as long; the interface is marked offline
   meanwhile, and announces sent in the gap are held for it.
2. **Both phones connect to each other, and each kept a different link.** Each
   refused the other's survivor as a duplicate; they reconnected every few
   seconds and no TAK traffic crossed. Now both keep the link whose central is
   the lower identity.
3. **A second connection to a rotated address tore down the shared link** about
   every 60 s, in step with advertising refresh. The local MAC is hidden on
   Android, so ordering by MAC could not work. Each phone now advertises the
   first 8 bytes of its identity in the scan response, and only the lower
   identity connects.
4. **Introduced by 1: a peer back inside the grace by a path the parent does not
   revive stayed offline.** Rev 1 reconnected in under a second, packets kept
   arriving, and Transport routed nothing to it for seven minutes. Now any kept
   interface with a connected address and no detach pending is online.

The MTU-20 observation above was only the pre-handshake value; links reach
509-512 once negotiated. Messaging and markers crossed phone to phone after 1-3.

**Resolved 2026-09-27**, measured with `tools/ble_link_soak.py`: the identity
tag dropped from the scan response after the first advertising refresh (fixed:
100 % online both sides, no disconnects over ten minutes); a status-133 retry's
leftover timeout and an unreadable identity each left a one-sided link; and
the GATT client handled only Android 13 callbacks, so an Android 8 phone as the
connecting side could send but never receive. Throughput between the A54 and
the Nexus 6P is ~50 kbit/s and bounded by the Nexus's radio -- one 488-byte
packet per ~60 ms, with or without a write response; see *PR E extended* in
`TAKDeliveryPlan.md`. Still owed: the same measurement between two current
phones.

## 12. The deck cannot reach REV2-2, though REV2-2 reaches the deck -- found 2026-10-08

The gateway's first reachability probes (reticulum-telemetry #11): Rev 1
answered every probe (0.76-2.2 s, 2 hops via UDP); **REV2-2 answered none**.
Reticulum's own `rnprobe` agrees -- 100% loss -- and `rnpath` gives the
deck's path to REV2-2's probe destination as **3 hops via REV2
(`b495d0cd`) on UDP**. Yet REV2-2's own telemetry reaches the gateway in 2
hops, and REV2 hears REV2-2 directly over LoRa (-51 dBm on the topology map).
So the mesh carries traffic from REV2-2 and not to it.

**Measured:** probe loss 100% (3 of 3 cycles, plus rnprobe); the deck's path,
3 hops via REV2; REV2-2's reports arriving, 2 hops.

**Not yet known:** which third hop the deck's path names, and whether it is a
route from before the 2026-10-08 power outage (REV2-2 was off 14:05-14:32,
and boards were moved between USB and wall) that a path request answered from
a cache. **First thing to try:** drop the deck's path (`rnpath -d`) and
request it again; read REV2's path table for REV2-2's destinations over its
NomadNet page. If the 3-hop route comes back, the reverse route is what is
wrong; if it does not, a stale path outlived its usefulness and a probe found
it.

**Why it matters for the field:** commands and messages to a board can fail
while everything the board sends still arrives, and nothing but a probe shows
it.
