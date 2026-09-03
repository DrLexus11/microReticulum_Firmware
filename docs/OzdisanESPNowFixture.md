# Özdisan ESP-NOW Acceptance Fixture

Status: ESP-NOW radio acceptance, NomadNet pages/subindexes, and the bondless
BLE peer path all pass. Verified against a scripted client standing in for
Columba; the phone itself has not been run against this build.

## Hardware identity

The source board definition is Meshtastic's
`variants/esp32/ozdisan_oled_rfm95` variant in the adjacent firmware checkout.
It identifies the board as an original ESP32-WROOM-32 using the
`esp32doit-devkit-v1` PlatformIO definition. The onboard SSD1306 wiring is:

| Signal | GPIO |
| --- | ---: |
| OLED SDA | 5 |
| OLED SCL | 4 |
| OLED reset | 16 |
| OLED address | `0x3c` |

Meshtastic's variant also describes an external RFM95. The available Özdisan
boards no longer have that module, so this target intentionally does not
compile `LORA_TRANSPORT` and never drives the old radio pins.

The local PlatformIO environment is `ozdisan-esp32-espnow`. It uses board ID
`BOARD_OZDISAN_ESP32`, declares no LoRa hardware, enables ESP-NOW and the BLE
peer transport, and gives the 4 MB flash a no-OTA partition layout. The target
uses `PRODUCT_HMBRW`, `MODEL_FE`, hardware revision 3 for its signed device record.
Its device, fallback-AP, DHCP-host and default NomadNet label is `OZD-ARD-01`.

## Intended boot behavior

On erased local configuration the fixture seeds WiFi mode to station, but no
SSID or password. It therefore cannot associate with the home network. Its
target-specific default enables the bounded `scan-before-softap` recovery
policy; the normal RAD Rev1 and Rev2 defaults remain off.

The first failed station check asks the ESP-NOW interface to start immediately.
It visits the configured rendezvous channel first and then sweeps the legal
2.4 GHz channels. A valid nonce/IFAC proof pins it to the responder's channel.
At that point Reticulum traffic can cross ESP-NOW even though the fixture has
no infrastructure WiFi and no LoRa. A RAD peer can forward that traffic over
its own LoRa interface. Because this fixture has no configured SSID, it does
not abandon a healthy pinned peer for the generic periodic station retry. It
stays on the selected channel while discovery from the already proven MAC or
accepted Reticulum traffic keeps that peer alive. Loss of the selected peer
for the peer timeout ends the pin and allows the normal recovery/fallback flow.

If no proven peer is found within the scan budget, the existing SoftAP fallback
still runs. This is intentional: the fixture remains locally recoverable rather
than becoming unreachable.

## Build, provision, and flash

Build without touching hardware:

```sh
pio run -e ozdisan-esp32-espnow
```

For a new board, first flash the image so the RNode KISS firmware is running,
then write its one-time signed identity. Normal uploads preserve that identity.
Use the exact serial path discovered after attaching the board:

```sh
pio run -e ozdisan-esp32-espnow -t upload --upload-port /dev/ttyUSBX
pio run -e ozdisan-esp32-espnow -t provision --upload-port /dev/ttyUSBX
```

This target uploads at 115200 baud. Its CP2102 connection proved unreliable at
460800 through the Steam Deck's shared hub and once disconnected during an app
partition write; the conservative rate completed and verified the same image.
This acceptance target intentionally does not pin its signed device record to
one application-image hash. Otherwise every legitimate development rebuild
fails `device_init()`, disables TNC transport and displays `FIRMWARE CORRUPT`
until `fixhash` runs. Production RAD targets retain firmware-image validation;
OZD still validates its EEPROM device record and retains its node identity.

Do not guess the port when Rev1 is attached at the same time. Match USB VID/PID,
serial number, and disconnect/reconnect behavior before writing. Provisioning
is a one-time operation because rewriting the locked identity is not harmless.

## Three-board acceptance topology

- Rev2 stays on wall power, associated to infrastructure WiFi, with ESP-NOW on
  that WiFi channel and LoRa available.
- Rev1 is attached by native USB for logs and acts as the known-good observer.
- Özdisan is attached through its UART bridge for flash/logging, stays off the
  infrastructure network, and discovers Rev1 or Rev2 by recovery sweep.

Acceptance evidence should show:

1. The fixture reports recovery `pinned`, the selected peer MAC, and the same
   channel as the infrastructure-connected RAD.
2. Both ends increment accepted ESP-NOW packet counters, not merely discovery
   counters.
3. A bidirectional Reticulum request/response or LXMF message succeeds with the
   fixture absent from the home WiFi client list.
4. With the directly reachable RAD path removed, traffic from the fixture can
   be observed leaving the selected RAD over LoRa.
5. A mismatched backbone IFAC key does not pin and increments proof failures.
6. Removing all responders completes the bounded scan and raises SoftAP.

The NomadNet `/page/espnow.mu` page is the primary on-mesh diagnostic surface.
On this fixture, `Local PHY` correctly reports `none`; a peer's discovery can
still report its LoRa PHY hash.

## Hardware acceptance record

The attached fixture was identified as an ESP32-D0WDQ6 revision 1.0 behind a
CP2102 bridge (MAC `40:91:51:9b:2d:d0`). Rev1 was the infrastructure-connected
responder (MAC `80:b5:4e:f4:c7:a4`). The home AP changed between channels 8,
9, and 10 during testing; each fresh recovery sweep found and pinned Rev1 on
the channel it was actually using.

An open fixture correctly refused a protected Rev1 responder. After enrolling
the fixture in the same published lab IFAC vector, the nonce/proof handshake
succeeded. That first success uncovered a wraparound bug: the loop timestamp
was captured before inbound handling, while the proof handler recorded a
slightly newer `last_seen`; unsigned subtraction made the peer appear almost
2^32 milliseconds old and immediately selected SoftAP. Refreshing the time
snapshot after draining inbound frames fixes recovery, peer, and reassembly
expiry together.

The final retained-handle run started both ESP-NOW counters from zero, pinned
the fixture on channel 9, and forced normal Reticulum traffic through the
post-boot announce schedule. The resulting counters were:

| Endpoint | Packets in | Packets out | IFAC accepted | Accepted from selected | Recovery |
| --- | ---: | ---: | ---: | ---: | --- |
| Özdisan | 1 | 1 | 1 | 1 | pinned, 0 failures |
| Rev1 | 1 | 3 | 1 | n/a | strict responder, 0 send failures |

This proves bidirectional, IFAC-protected Reticulum packet exchange while the
fixture has no configured infrastructure SSID and no LoRa hardware. Separate
negative runs also proved bounded no-peer fallback to SoftAP.

The next-morning check found both RAD IPs reachable with no ping loss and both
TCP RNS servers open. A fresh path request resolved the fixture's NomadNet
destination three hops away through wall-powered Rev2, consistent with the
intended Rev2/LoRa/Rev1/ESP-NOW topology. An encrypted NomadNet link still
closed during establishment (`status=4`), however, so an application-level
page request or LXMF round trip is not claimed as passed. Track that alongside
the existing direct-RNode link/identity instability rather than weakening the
radio-layer acceptance above. This describes the earlier overnight check; the
later reverse-path acceptance below supersedes its application-layer result.

The current OZD image was verified after an ordinary flash with `hw_ready=1`,
TNC mode and RNS transport enabled; no post-flash hash repair is required.
The compact profile intentionally omits the ns108 device-metrics namespace.
The reduced ns108 subset still cost enough internal heap to reproduce watchdog
resets on this no-PSRAM target; the periodic serial heap and reset diagnostics
remain available while the fixture is attached for testing.


## Reverse-path acceptance: outside to inside over BLE

The forward direction — the deck loading NomadNet pages from the OZD board over
ESP-NOW — passed first. The reverse is the one the product needs: a client with
no route into the mesh of its own arrives over BLE and is carried inward.

Run on 2026-09-02 with `tools/ble/BLEPeerClientInterface.py` as the client, in a
Reticulum instance whose only interface is that BLE link, so no result can have
arrived by another route.

| Stage | Evidence |
| --- | --- |
| Service on air | `40:91:51:9B:2D:D2 name='OZD-ARD-01' rssi=-63` advertising `37145b00-…-8f42c5da28e3` |
| GATT contract | identity `…28e6` read, tx `…28e4` read+notify, rx `…28e5` write+write-no-response |
| Bondless | `inbound peripheral link … connected (handle=0, unpaired)`; BlueZ reports `Paired: no  Bonded: no` |
| Identity handshake | `[blepeer] accepted peer identity <31d614cf…> over unpaired GATT` |
| Reticulum path, 1 hop past the node | Rev 1 `<ba03aa75…>` 2 hops via `<0056bb6a…>` |
| Reticulum path, across LoRa | Rev 2 `<41fc2ab5…>` 3 hops via `<0056bb6a…>` |
| Application data | 544-byte NomadNet page fetched from Rev 2, `Verified identity` line present |
| Reconnect | client reattached and re-handshook 1.6 s after a firmware reflash and reboot |

The hop counts are the load-bearing part. Rev 2 at three hops is
`BLE → OZD → ESP-NOW → Rev 1 → LoRa → Rev 2`. The deck also holds UDP interfaces
to both RAD boards, and had the reply come back that way Rev 2 would have read
four hops. Three means the LoRa hop carried it.

The page fetch returning `Verified identity` also shows the client's identity
reached Rev 2 intact across BLE, ESP-NOW and LoRa, so `ALLOW_LIST` gating passed
rather than being bypassed.

### Heap, measured rather than assumed

The board is an original ESP32 with no PSRAM, and heap — not flash — is what
constrains this image. Flash sits at 54% of the 2.75 MB app slot. Boot probes:

```
Total SRAM              216288
post-nimble-init        133616      NimBLE controller + host   19,464 B
post-wifi-init           83560      Wi-Fi                      50,056 B
post-new-espnow          71812      ESP-NOW                     9,480 B
post-provisioning        66380      compact config only         4,656 B
post-reticulum-start     43684      RNS                        22,544 B
steady state         51440-55428      largest block 29,684-32,756
```

Wi-Fi is the largest single consumer, at two and a half times NimBLE. With
roughly 83 KB free after Wi-Fi and 44 KB after Reticulum has started, a BLE host
costing what Bluedroid costs does not fit, which is why this target uses NimBLE
while the RAD boards keep the proven Bluedroid backend.

The ns108 decision was made with a controlled hardware A/B. The reduced metrics
registry left 58,480 bytes after provisioning and 37,844 after RNS start, then
repeatedly reset under `TASK_WDT` in loop phase 10 within roughly 60-90 seconds.
With only ns108 removed, the same board and network ran for 600 seconds on one
boot. During that run it recovered Rev1 on ESP-NOW channel 9, processed repeated
NomadNet re-announces, and survived four failed outbound BLE connection attempts.
Its steady free heap stayed in the range shown above. That makes the metrics
registry an unacceptable cost for this fixture even though it remains useful on
the PSRAM-equipped RAD targets.

### Multi-peer BLE capacity

The OZD NimBLE backend is a bounded multi-access interface rather than a
single-client modem. It defaults to the same seven-peer capacity as Columba,
with both the firmware slot count and NimBLE controller count set by build
flags. Each connection owns its identity and fragment reassembly state;
outbound Reticulum frames fan out to every ready peer. Advertising is re-armed
after every accepted connection and remains active until all slots are used.

Raising the controller capacity from its library default of three to seven
costs about 5 KB of free heap on this board. The resulting image measured
37,440 bytes free after Reticulum start and about 48,800 bytes at 60 seconds,
with a largest block around 31,700 bytes. A 90-second hardware run recovered
Rev1 over ESP-NOW and did not reset. This is an initial safety check, not yet an
overnight multi-client soak.

A bondless deck client has also connected while the node continued advertising,
read the 16-byte identity, subscribed, completed its identity write and received
Reticulum traffic. The current real-phone check is narrower: Columba advertises
a valid, readable service, but OZD's outbound central attempt to the observed
Android advertiser times out with NimBLE reason 13. The always-available OZD
peripheral path is therefore retained for Columba to initiate; phone-side logs
are still needed to determine why that discovery has not produced an inbound
connection in the current test.


## Columba acceptance: the reverse path, end to end

Run 2026-09-03 with the real Columba client (debug variant `columbatest`,
package `network.columba.app.debug`), production Columba stopped, only the
Bluetooth LE peer interface enabled.

```
08:09:19.287  Discovered new device: C0:91:51:9B:2D:D2 (OZD-ARD-01) RSSI: -70
08:09:19.601  Peer connected (central=false, peripheral=true, dedupe=NONE, MTU=20)
08:09:20.413  Peer connected (central=true,  peripheral=true,
                              dedupe=CLOSING_PERIPHERAL, MTU=509)
08:09:20.429  Deduplication complete: peripheral connection closed
08:09:23      BLEPeerInterface[OZD-ARD-01]=online
08:12:06      RX: 231 bytes from OZD-ARD-01     (link held ~3 min and counting)
```

Traffic over the link: 3592 B out, 4313 B in. Announces arrived for two mesh
destinations, `cd1da3a1545c217a` and `ed03deced0c93e38` -- the second is two
hops away via `60ba52911ee1ddbefc646a67dc969894`, Rev 1's transport identity, so
it originated beyond Rev 1. That is the whole chain carrying live Reticulum:

```
mesh -> Rev 1 -> ESP-NOW -> OZD-ARD-01 -> BLE -> phone
```

### Why it works now

The board takes a stable static random address derived from its factory MAC
(`c0:91:51:9b:2d:d2`), which sorts above any phone address, so it declines to
dial and waits. Previously its Espressif public address `40:91:51:..` sorted
below a phone's, so both sides dialled at once; Columba saw one address holding
a central and a peripheral role and closed one of them. Yesterday it closed the
only usable link. Note the collision still occurs -- the trace above shows both
roles -- but with the board waiting, the surviving link is the 509-byte central
one rather than the 20-byte peripheral one, because `preferredBleRole()` selects
on MTU. See `docs/Backlog.md` items 12 and 13.


## Measurement hazard: attaching to serial used to reset the board

Watching the log rebooted the thing being watched, and it was not obvious.

`serial.Serial(port, ...)` opens the port immediately, and pyserial asserts DTR
and RTS by default while doing so. The kernel then lowers them again on close
unless `HUPCL` is cleared. Either edge is a reset pulse on EN for this board's
CP2102 auto-reset circuit, so both attaching and detaching restarted the node.

**It is visible on the OLED**, which blanks for about a second around every
attach. If the display flickers when a log command runs, that is a reboot, not a
redraw.

What it cost, before it was understood: consecutive captures kept showing a
freshly booted board; uptime appeared to fall between samples; and a capture that
happened to catch the quiet window before the first log line looked exactly like
a hung node. Each of those was read as a firmware fault at least once.

`tools/ozd_serial_log.py` now sets the line states before `open()` and clears
`HUPCL` on its own descriptor. Verified by attaching three times in succession
and reading uptime 60 s, 120 s, 180 s, where the same sequence previously reset
the board each time.

The same hazard applies to any tool that opens these ports. `provnoreset.py` was
written for this reason; prefer it, or this viewer, over a bare `serial.Serial`.
