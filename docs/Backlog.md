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
