# Özdisan ESP-NOW Acceptance Fixture

Status: firmware target implemented and host-built; hardware acceptance pending.

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
its own LoRa interface.

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
