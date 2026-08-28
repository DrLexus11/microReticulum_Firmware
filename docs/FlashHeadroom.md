# Flash Headroom

Where the application partition is going, what is already optimised, and the
work queued to stop flash being a design constraint.

Status: **measured 2026-08-27 on Rev 1 and Rev 2.** Repartition and the
low-hanging items are scheduled ahead of the Bluetooth overhaul in
[`docs/FeatureRoadmap.md`](FeatureRoadmap.md).

---

## 1. Where we are

| Environment | Used | Partition | % |
| --- | ---: | ---: | ---: |
| `impr-rad01-rev1` | 1,836,865 | 2,097,152 | 87.6% |
| `impr-rad01-rev2` (USB) | 1,891,309 | 2,097,152 | **90.2%** |
| `impr-rad01-rev2-uart` | 1,852,417 | 2,097,152 | 88.3% |

## 2. What is already optimised

Checked before proposing anything, because these are the obvious first
suggestions and both are already in place:

- **`-Os` is active.** The compile line carries `-O0` and then `-Os`; GCC honours
  the last such option, so the build is size-optimised despite appearances.
- **`--gc-sections` is in the link**, supplied by the framework, paired with the
  project's `-ffunction-sections` and `-fdata-sections`.

There are no free compiler-flag wins. Anyone reading the `-O0` on the command
line and proposing `-Os` has found the same false lead.

## 3. The levers, largest first

### 3.1 Repartition — up to +1 to +2 MB

The boards carry **8 MB of flash and the partition table maps only 4 MB**:

```
app0      0x10000   0x200000   2.0 MB    90% full
spiffs    0x210000  0x1E0000   1.9 MB    83% empty
coredump  0x3F0000  0x10000
                    ------------------
                    4 MB of an 8 MB chip
```

So the application is squeezed into 2 MB while roughly 4 MB of the chip is
unmapped and most of the filesystem beside it is unused.

| Option | App | Filesystem | Verdict |
| --- | ---: | ---: | --- |
| `huge_app.csv` | 3 MB | 896 KB | +1 MB, still wastes 4 MB |
| **Custom 8 MB table** | **3-4 MB** | **3-4 MB** | uses the whole chip |
| `max_app_8MB.csv` | 7.9 MB | none | unusable, LittleFS is required |

**Cost.** Repartitioning erases LittleFS, which holds provisioning
configuration and the LXMF message store. NVS stays at `0x9000` in every
candidate table, so the device identity and EEPROM survive. This needs a config
backup and restore, and a reflash of both boards -- batch it with other
boot-mode work rather than doing it alone.

### 3.2 Bluetooth: Bluedroid to NimBLE

`libbt.a`, `libBLE.a` and `libbtdm_app.a` are the largest code blocks in the
link map by a wide margin, and Bluedroid is compiled into both boards today.
See [`docs/BluetoothOverhaul.md`](BluetoothOverhaul.md): the port is expected to
*return* headroom rather than consume it.

**Measured, on `impr-rad01-rev1`, by building with and without the stack:**

| | Flash | RAM |
| --- | --- | --- |
| With Bluedroid | 1,851,873 (44.2%) | 114,636 (35.0%) |
| No Bluetooth stack | 1,257,857 (30.0%) | 86,928 (26.5%) |
| **Difference** | **594,016 (580 KB)** | **27,708 (27 KB)** |

Map-file attribution had been attempted twice and produced obvious nonsense --
44 MB attributed to a 1.8 MB image -- because the map includes discarded and
non-flash sections. Building both images is the measurement that actually works,
and it needed 3.3 fixed first.

What this does and does not say: 580 KB of flash and 27 KB of RAM is the cost of
*all* Bluetooth, so it is the **ceiling** on what a Bluedroid-to-NimBLE port can
return, not the expected saving. NimBLE has to fit somewhere inside that
figure. Quote it as a bound.

### 3.3 `RAD01_NO_BLE` -- fixed

It did not build: `Remote.h` used `bt_devname` for the SoftAP SSID and the DHCP
hostname while that symbol lived inside the `HAS_BLUETOOTH || HAS_BLE` guard,
and `RNode_Firmware.ino` referenced `SerialBT` from a branch that a Wi-Fi-only
image still compiles. The device-identity globals now sit outside the guard,
where they belong, and the Bluetooth arm of the read loop is compiled only when
a stack exists.

**One thing that fix uncovered, worth reading before trusting the target.**
`Device.h` compiled `dev_bt_mac` out of the device hash when no Bluetooth stack
was present, so a Wi-Fi-only image computed a *different* `dev_hash` than a
Bluetooth image on the same board. The Ed25519 device signature would then fail
to verify and the unit would come up unvalidated -- presenting exactly like the
firmware-hash failure that has already cost this project days: healthy over
Wi-Fi, deaf on RF. It is now hashed unconditionally, so both images agree.

Related, and separate: `dev_bt_mac` is never assigned on any build. It is six
zero bytes, so the device identity is not bound to the Bluetooth MAC despite
what the name and `docs/BluetoothOverhaul.md` imply. That must not be "fixed" --
every provisioned board's signature was computed over a hash containing those
zeros.

### 3.4 RNS log level -- 38,892 bytes, measured

The only material difference between `impr-rad01-rev2` (90.2%) and
`impr-rad01-rev2-uart` (88.3%) is `-DRNS_LOG_LEVEL=RNS_LOG_LEVEL_ERROR`. Log
strings cost roughly 39 KB.

Available immediately, and **not recommended yet**. The crash loop that cost a
day was found because the board could still describe itself; trading diagnostics
for 2% is a poor exchange until the partition work is done.

### 3.5 Link-time optimisation -- untested

`-flto` exists in `platformio.ini` but is commented out. Typically 5-10% on C++,
at the cost of longer builds and a real chance of exposing latent undefined
behaviour. Worth one measured experiment before it is trusted, not a default.

## 4. Order of work

1. **Repartition to a custom 8 MB table.** Largest gain, and cheap because the
   filesystem beside the app is mostly empty.
2. **Fix `RAD01_NO_BLE`.** Small, repairs a documented target, and unblocks an
   honest measurement of the Bluetooth stack.
3. **NimBLE port**, per `docs/BluetoothOverhaul.md`, now with a measured
   argument behind it.
4. Hold log level and LTO in reserve.

Items 1 and 3 together should take the application from 90% of 2 MB to roughly
50-60% of 3-4 MB, which removes flash as a design constraint for the
foreseeable future.

## 5. The migration, as performed

Executed on Rev 1 on 2026-08-27. Recorded because the obvious approach --
reflash and reconfigure -- would have quietly destroyed the node's identity.

**The trap.** The Reticulum transport identity is a *file on LittleFS*
(`Transport.cpp`: `"%s/transport_identity"`), not EEPROM or NVS. Repartitioning
moves the filesystem, so a naive repartition regenerates it and every
destination hash on the node changes: the RRC hub, the LXMF propagation node and
the NomadNet site. Every client configuration pointing at those hashes breaks,
and the "stable across reboots and firmware updates" property that PR 3
acceptance tested is lost.

**Why the filesystem size is unchanged.** `boards/rad01_8mb.csv` keeps the
filesystem at exactly `0x1E0000`, the same as `no_ota.csv`. Identical size means
the partition can be copied byte for byte to its new offset. A resized
filesystem could not be: LittleFS geometry is fixed at format time.

**Sequence.**

```console
# 1. Back up the filesystem BEFORE flashing anything
esptool --port <port> --chip esp32s3 --baud 921600 \
        read_flash 0x210000 0x1E0000 fs_backup.bin

# 2. Verify the backup really holds the identity, not a blank read
strings -n 4 fs_backup.bin | grep transport_identity

# 3. Flash firmware and the new partition table
pio run -e <env> -t upload --upload-port <port>

# 4. Restore the filesystem at its new offset
esptool --port <port> --chip esp32s3 --baud 921600 \
        write_flash 0x410000 fs_backup.bin
```

Step 2 is not optional. A `read_flash` of an unmounted or wrong region returns a
plausible-looking file of the right length; checking for the identity by name is
what distinguishes a real backup from 1.9 MB of `0xFF`.

The board boots once between steps 3 and 4 and briefly formats an empty
filesystem, generating a throwaway identity. Step 4 overwrites it. Harmless, but
do not stop between those steps and assume the node is fine -- it is announcing
under a hash nobody knows.

**Result on Rev 1.** Application 87.6% of 2 MB to **43.8% of 4 MB**. RRC hub
`d36d1371772fca94fb6dc2522d1c4254`, LXMF propagation node
`ba03aa75f8a136b1b6a74667c755727e`, radio configuration and RRC settings all
unchanged. Two-identity RRC acceptance passes. Loop stack margin unaffected.

**Result on Rev 2.** Same sequence, application 88.3% of 2 MB to **44.2% of
4 MB**. RRC hub `736043f85ba10cd1b8f01b6726c7bee9`, LXMF propagation node
`41fc2ab5e88d0b355d3c35fa60f4a22e`, and the message store reloaded with exactly
its previous contents -- `store loaded: 12 message(s), 2992 bytes`. That last
line is the clearest proof the copy was byte for byte: a reformatted filesystem
would have reported an empty store.

**One interruption worth recording.** Between flashing the table and restoring
the filesystem, Rev 2 went unreachable -- no bootloader, no serial, no ping,
which on this bench has meant the board working loose on the breadboard. That is
exactly the window this document warns not to linger in, and the recovery was
uneventful only because the backup already existed and had been verified. Take
the backup first, verify it by name, and keep it somewhere that outlives the
session: a scratch directory that is cleaned up would have turned a reseat into
a lost identity.

## 6. Where this leaves flash

| Environment | Before | After |
| --- | ---: | ---: |
| `impr-rad01-rev1` | 87.6% of 2 MB | **43.8% of 4 MB** |
| `impr-rad01-rev2` | 90.2% of 2 MB | **45.1% of 4 MB** |
| `impr-rad01-rev2-uart` | 88.3% of 2 MB | **44.2% of 4 MB** |

2 MB of the 8 MB chip remains unallocated beyond the coredump partition, so
there is room to grow the filesystem later without touching the application
again -- though doing so reformats it, and §5 applies.

Flash is no longer the binding constraint. The Bluetooth overhaul in item 4 of
the roadmap now has somewhere to land, and the remaining trims in §3.4 and §3.5
can stay unspent.
