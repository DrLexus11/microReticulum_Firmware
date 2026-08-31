# ESP-NOW Peer Interface

Implementation contract for the one-hop ESP-NOW link exposed to Reticulum by
`ESPNowInterface.h`.

Status: **implemented on `feature/esp-now-peer`; hardware acceptance pending.**

## Boundaries

ESP-NOW discovers and reaches radios on the current 2.4 GHz WiFi channel. It
does not calculate paths, repeat an ESP-NOW frame, or maintain a second routing
table. A fully reassembled frame is delivered to Reticulum as input on an
ordinary gateway interface; Reticulum alone decides whether and where to send
it next.

Traffic is broadcast at the ESP-NOW link layer. A RAD can therefore hear all
adjacent RADs on the same channel without a provisioned peer list. Reticulum's
packet hashes provide the normal duplicate suppression above this link.

## WiFi coexistence

The interface follows the active ESP32 radio channel (`channel = 0` in the
ESP-NOW broadcast peer). It never scans or changes channel:

- a RAD associated to home/infrastructure WiFi can discover another RAD only
  when both are on that same router channel;
- AP+STA mode uses the station interface and the single channel shared by both
  virtual interfaces;
- AP-only mode uses the AP interface;
- changing WiFi mode restarts ESP-NOW, while a channel change is followed
  automatically.

This preserves the RAD's normal WiFi connection. It also means ESP-NOW is not
a way to find every 2.4 GHz node across channels simultaneously.

## Wire format, version 1

Every ESP-NOW payload begins with this ten-byte, big-endian header:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 2 | Magic `RN` (`52 4e`) |
| 2 | 1 | Protocol version (`01`) |
| 3 | 1 | Type: discovery `01`, Reticulum data `02` |
| 4 | 2 | Sender-local packet ID |
| 6 | 1 | Zero-based fragment index |
| 7 | 1 | Fragment count |
| 8 | 2 | Total reassembled payload length |

ESP-IDF 4.4 permits 250 application bytes. The header leaves 240 bytes per
fragment. The interface accepts Reticulum frames up to 564 bytes (the normal
500-byte packet plus the maximum 64-byte IFAC), requiring at most three
ESP-NOW frames. Fragment sequences are strict and in order per source MAC; an
incomplete sequence expires after three seconds.

Discovery is a single 16-byte payload containing, in order: LoRa PHY hash
(`u32`), frequency (`u32`), bandwidth (`u32`), spreading factor, coding rate,
current WiFi channel, and capability flags. It is emitted every 10 seconds plus
up to two seconds of jitter. A peer remains recent for 45 seconds.

Discovery data is unauthenticated and advisory. It exists to show an operator
that a nearby radio uses a different LoRa PHY and to support a later,
authenticated orphan-recovery design. This implementation never applies a PHY
change from a discovery frame.

## Security

ESP-NOW encryption is disabled because broadcast ESP-NOW cannot use per-peer
link encryption. Reticulum still supplies end-to-end cryptography. For
admission control, the interface shares the LoRa/backbone IFAC configuration:
the same network name, passphrase, eight-byte access code, and secure-node
fail-closed policy are applied to both radio links. Thus enabling backbone IFAC
does not accidentally leave ESP-NOW as an open ingress path.

## Runtime limits and observability

All callback-facing storage is fixed-size. The WiFi task only copies received
bytes into an eight-entry queue and records send completion; parsing,
reassembly, Reticulum calls, and logging execute later from `loop()`.

The implementation retains at most 12 recent peers, six concurrent
reassemblies, eight inbound wire frames, and four outbound Reticulum packets.
Overflow is dropped and counted. Provisioning namespace 116 reports interface
state, channel, peer count, packet/discovery counters, queue drops, send
failures, reassembly timeouts, and the most recently observed LoRa PHY hash.

## Hardware acceptance

Before merging to production, exercise Rev1 and Rev2 both associated to the
same infrastructure network and again in the no-router fallback configuration:

1. Both nodes report the same non-zero WiFi channel and at least one recent
   peer within 12 seconds.
2. Announces and bidirectional Reticulum packets cross ESP-NOW with LoRa
   temporarily isolated.
3. A maximum-size IFAC frame crosses in three fragments.
4. WiFi association, TCP/UDP access, and watchdog health remain stable under
   sustained ESP-NOW traffic.
5. Different-channel nodes do not claim discovery, and malformed/incomplete
   sequences increase drop/timeout metrics without resetting either board.
