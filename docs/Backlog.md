# Backlog

Known, measured, and deliberately not fixed yet. Each entry says what was
observed rather than what was suspected, so picking one up does not start with
re-deriving the problem.

---

## nRF52 images overflow flash by ~6.7 KB

`pio run -e wiscore_rak4631` compiles cleanly and then fails to link:

```
ld: region `FLASH' overflowed by 6688 bytes
```

Found while proving the portability fix for the time-sync nonce (the header is
compiled under `HAS_RNS`, which every board defines). Compilation is now fixed;
the size is not.

**Not caused by the time work, and not newly broken.** The nRF52 build could not
compile at all before this branch — `boot_rail_lost` and `boot_prev_uptime` are
defined inside `#if MCU_VARIANT == MCU_ESP32` but were used unguarded, from
commit `8c13325`, which is on `master`. So no one has linked an nRF52 image
since at least that commit, and there is no known-good baseline to compare the
overflow against.

Worth doing before claiming nRF52 support at all: establish whether the fleet
still wants these targets. If it does, the first step is measuring which
features actually cost the flash, not trimming blindly.

---

## The USB-JTAG control lines have no explanation, only measurements

Three tools drive DTR/RTS on the ESP32-S3's native USB-Serial/JTAG, and two of
them need opposite DTR states:

| tool | talks to | DTR | measured |
|---|---|---|---|
| `extra_script.py` post-upload | ROM | deasserted, pulse RTS | starts the application, verified across repeated flashes |
| `tools/usb_jtag_boot.py` | ROM | deasserted, pulse RTS | recovers a board from the downloader |
| `tools/ifac/provision.py --usb-jtag` | the running app | **asserted**, no pulse | answers immediately; deasserted times out *and leaves the board in the ROM downloader* |

Every row is measured on IMPR-RAD-01-REV2-2 (N16R2, 303a:1001). What is missing
is a mechanism that explains all three at once. An earlier attempt to write one
down ("RTS drives EN, DTR drives IO0, both inverted") is contradicted by the
third row, so the comments now state the measurements and say so.

Until someone reconciles this against the ESP32-S3 TRM, treat the table as the
contract and change any of it only with a board in hand.

---

## `Cryptography::randomnum()` yields eight bits of entropy

In `microReticulum`, `src/microReticulum/Cryptography/Random.h`:

```cpp
uint32_t randnum = uint32_t((unsigned char)(rand.data()[0]) << 24 |
                            (unsigned char)(rand.data()[0]) << 16 |
                            (unsigned char)(rand.data()[0]) << 8  |
                            (unsigned char)(rand.data()[0]));
```

Four bytes are drawn and byte zero is used four times, so the result takes 256
values rather than 2^32. `randomnum(max)` inherits it.

Avoided rather than fixed here: `TimeSync.h` draws `random(8)` and assembles the
nonce itself. Anything else calling `randomnum()` for anything security-bearing
should be audited before this is closed, and the fix belongs upstream.

---

## The ESP-NOW send drain is unproven

Landed, bounded, and never demonstrated to matter. It should either be shown to
change a measurement or removed.

## Rev 1's occasional wedge has no root cause

The PSRAM change removed the fragmentation mechanism that could be measured; it
does not prove that was the only one.

## The persisted store index loads unbounded, then prunes

`microStore::BasicFileStore::init()` (in `attermann/microStore`,
`include/microStore/FileStore.h`) does this, in order:

```cpp
if (!load_index()) rebuild_index_from_segments();
sweep();
size_t pruned = prune_index_to_max_recs();
```

`load_index()` replays the entire on-flash index into a RAM `unordered_map`.
Only after that is the map cut down to `max_recs` -- 50 for the packet
hashlist. So the peak RAM cost is the size of the store's *history*, not its
configured maximum, and a node that has been up for days pays it at
`Reticulum::start()` before anything else has run.

OZD-01 hit the ceiling on 2026-09-06: `Reticulum::start()` needs about 15 KB
with empty stores and had 37 KB, and its accumulated path, known-destination
and hashlist stores took the rest. The failing allocation was 32 bytes inside
`sweep()`, which throws `std::bad_alloc` from a container with nothing to catch
it -- `__cxa_allocate_exception` then could not allocate either, so it went
straight to `std::terminate`. Eleven aborts in eighty seconds, and the board
read the same store again on every one.

`sweep()` makes it worse than it needs to be:

```cpp
std::vector<KeyType, KTSAlloc> evict(kts_alloc);
evict.reserve(_index.size());
for (auto& kv : _index) if (kv.second.timestamp > current_time) evict.push_back(kv.first);
for (auto& key : evict) _index.erase(key);
```

Every evicted key is *copied* into a second container before anything is
erased, so the moment of peak memory is the one where the store is already too
big. It needs no allocation at all:

```cpp
for (auto it = _index.begin(); it != _index.end(); ) {
    if (it->second.timestamp > current_time) { it = _index.erase(it); ++evicted; }
    else ++it;
}
```

And a clockless node evicts everything on every boot regardless: `sweep()`
drops records timestamped in the future, `microStore::time()` on a node with no
wall clock is uptime plus a persisted offset, and a fresh boot's uptime is
lower than the one the records were written at. So the board with no time
source -- the one least able to spare the RAM -- takes the whole-store path
every time.

Worked around in this repo, not fixed: `node_clear_persisted_caches()` deletes
all three stores after `NODE_CACHE_WIPE_FAULTS` consecutive faulted boots, so
the node recovers on its own instead of looping forever. It still pays the
airtime to relearn every path. Closing this properly means a fork of
microStore: bound the index at load, drop the copy in `sweep()`, and stop
treating a clockless node's uptime as a wall clock.
## On-board GNSS (GP-02)

Deferred as a feature of its own on 2026-09-06, written up in
[`OnboardGNSS.md`](OnboardGNSS.md). It was previously the first two steps of the
TAK plan on the strength of the clock argument in `TAKCapability.md` §5; time
propagation shipped and that argument no longer holds. What remains is the
unattended node — a relay with no phone attached cannot report a position, and
cannot get the time when the mesh is partitioned away from its authorities.

Not blocking. Pin questions are answerable from
`~/projects/kicad_labs/lab6_mcu_lora/rev2/IMPR-RAD-01/`.

## Two time-on-air figures that disagree by twenty percent

`TAKCapability.md` §2 puts a compact position report at 44 ms on air at
SF7/BW250. `tools/position_budget.py`, which uses the firmware's own
`packet_airtime_ms()` arithmetic, computes 53 ms for the same 60 bytes. The CoT
comparison agrees closely (538 ms documented, 550 computed), so the models are
not wildly apart -- but the compact figure is out by a fifth.

The firmware's formula adds `preamble + 0.25 + 8` symbols where the SX127x
datasheet's `T_preamble` uses `preamble + 4.25`. That may be a deliberate
folding of terms or an inherited off-by-four; it has not been traced. It is
used consistently for the duty-cycle accounting, so nothing is inconsistent
*within* the firmware, and both figures are far enough inside the budget that
no decision made so far turns on it.

Worth resolving before anyone sizes a fleet close to the limit. The node is the
one to believe either way, since it is the thing actually transmitting.


## The command post has no radio of its own

`~/.reticulum/config` on the deck reaches both RADs over `UDPInterface` to
192.168.1.x. That is the LAN, and it does not exist outdoors, so the command
post silently has no path into the mesh the moment it leaves the building.

Needs an `RNodeInterface` on USB serial. Scheduled as PR 3 in
[`TAKFieldExercise.md`](TAKFieldExercise.md) §4; recorded here because it is a
live misconfiguration rather than only a future feature, and anyone taking the
deck outside today would find it the hard way.

## Columba cannot bridge a tailnet to LoRa

Two things stop a phone acting as the command post's way onto the mesh:
`enable_transport = No` in the config it generates, and no UI for adding a
TCPServerInterface on a chosen address. Both are needed for the Tailscale arm
of the field exercise. Scheduled as PR 3.
