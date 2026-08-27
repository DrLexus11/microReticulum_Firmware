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

**The exact figure is not yet measured, and should not be quoted.** Map-file
attribution was attempted twice and produced obvious nonsense -- 44 MB
attributed to a 1.8 MB image -- because the map includes discarded and non-flash
sections. The reliable measurement is a build without Bluetooth, which is
blocked by 3.3.

### 3.3 `RAD01_NO_BLE` does not compile

`Boards.h` documents a WiFi/USB-only image via `RAD01_NO_BLE`, and it does not
build: `Remote.h` uses `bt_devname` for the SoftAP SSID and the DHCP hostname,
but that symbol is only declared inside the `HAS_BLUETOOTH || HAS_BLE` guard in
`Utilities.h`. The device name is not a Bluetooth concern and should not live
behind that guard.

Fixing it repairs a documented target, gives deployments that do not need
Bluetooth an immediately smaller image, and is the prerequisite for measuring
3.2 honestly.

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
