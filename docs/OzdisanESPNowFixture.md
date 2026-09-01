# Özdisan ESP-NOW Acceptance Fixture

Status: radio-layer hardware acceptance passed on 2026-08-31; the NomadNet
link/request check remains open as a separate higher-layer issue.

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
`BOARD_OZDISAN_ESP32`, declares no LoRa hardware, enables ESP-NOW and the TCP
server, and gives the 4 MB flash a no-OTA partition layout. The target uses
`PRODUCT_HMBRW`, `MODEL_FE`, hardware revision 3 for its signed device record.
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
pio run -e ozdisan-esp32-espnow -t fixhash --upload-port /dev/ttyUSBX
```

This target uploads at 115200 baud. Its CP2102 connection proved unreliable at
460800 through the Steam Deck's shared hub and once disconnected during an app
partition write; the conservative rate completed and verified the same image.

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
radio-layer acceptance above.

The fixture also boots more slowly than the four-second post-upload firmware
hash delay used by the RAD boards. A hash write at that point is lost and the
next boot reports `hw_ready=0`. Its upload/fixhash path now waits 20 seconds
before writing, and hardware was restored and verified with `hw_ready=1`, TNC
mode, and transport enabled.
