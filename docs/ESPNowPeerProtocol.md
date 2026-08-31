# ESP-NOW Peer Interface

Implementation contract for the one-hop ESP-NOW link exposed to Reticulum by
`ESPNowInterface.h`.

Status: **one-hop transport and opt-in orphan recovery implemented on
`feature/esp-now-peer`; hardware acceptance pending.**

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

In normal operation the interface follows the active ESP32 radio channel
(`channel = 0` in the ESP-NOW broadcast peer). It never scans or changes a
channel while station WiFi is associated:

- a RAD associated to home/infrastructure WiFi can discover another RAD only
  when both are on that same router channel;
- AP+STA mode uses the station interface and the single channel shared by both
  virtual interfaces;
- AP-only mode uses the AP interface;
- changing WiFi mode restarts ESP-NOW, while a channel change is followed
  automatically.

This preserves the RAD's normal WiFi connection. It also means strict-mode
ESP-NOW is not a way to find every 2.4 GHz node across channels simultaneously.

An optional recovery mode changes that rule only after station fallback is
already due and the station remains disconnected. The radio then performs a
bounded active channel sweep before raising the existing SoftAP. A successful,
IFAC-proven response pins the orphan to that channel; a failed sweep falls
through to SoftAP unchanged. While pinned, ordinary Reticulum traffic can cross
ESP-NOW to the selected RAD and continue over that RAD's LoRa interface. The
node periodically leaves the recovered link to retry its configured station,
using the same retry interval as SoftAP fallback.

## Wire format, version 1

Every ESP-NOW payload begins with this ten-byte, big-endian header:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 2 | Magic `RN` (`52 4e`) |
| 2 | 1 | Protocol version (`01`) |
| 3 | 1 | Type: discovery `01`, Reticulum data `02`, recovery solicit `03`, recovery reply `04` |
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
that a nearby radio uses a different LoRa PHY. This implementation never
applies a PHY change from a discovery frame.

### Active recovery frames

A solicitation is one unfragmented four-byte random nonce. A recovery reply is
one unfragmented 28-byte payload:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 4 | Echoed solicitation nonce |
| 4 | 16 | Normal discovery record |
| 20 | 8 | Recovery admission proof |

The scanner visits the configured rendezvous channel first, then the legal
channels reported by the ESP32 country configuration. It dwells 900 ms on the
rendezvous channel and 260 ms on each other channel, repeating until its
provisioned budget expires. Every hop uses a fresh nonce. Responders add 20-100
ms jitter so several nearby RADs do not reply in lockstep.

The first valid response pins the channel and peer for 45 seconds. A discovery
from that MAC or a complete Reticulum packet from it that passes the interface's
IFAC handling refreshes the lease. Merely receiving an ESP-NOW data frame does
not. Expiry marks recovery failed so the established fallback logic can raise
SoftAP.

## Security

ESP-NOW encryption is disabled because broadcast ESP-NOW cannot use per-peer
link encryption. Reticulum still supplies end-to-end cryptography. For
admission control, the interface shares the LoRa/backbone IFAC configuration:
the same network name, passphrase, eight-byte access code, and secure-node
fail-closed policy are applied to both radio links. Thus enabling backbone IFAC
does not accidentally leave ESP-NOW as an open ingress path.

Active recovery also binds the solicitation nonce and complete discovery record
to that backbone IFAC key with HKDF and an eight-byte proof. The proof is
compared without an early exit. A protected scanner rejects an open responder,
an open scanner rejects a responder claiming a proof, and mismatched keys do
not pin. This is link admission evidence and replay protection for channel
selection; Reticulum's normal packet-level IFAC validation remains authoritative
for received traffic.

## Runtime limits and observability

All callback-facing storage is fixed-size. The WiFi task only copies received
bytes into an eight-entry queue and records send completion; parsing,
reassembly, Reticulum calls, and logging execute later from `loop()`.

The implementation retains at most 12 recent peers, six concurrent
reassemblies, eight inbound wire frames, and four outbound Reticulum packets.
Overflow is dropped and counted. Provisioning namespace 116 reports interface
state, channel, peer count, packet/discovery counters, queue drops, send
failures, reassembly timeouts, the most recently observed LoRa PHY hash,
recovery state/results/proof and channel errors, selected peer/channel, and
IFAC-accepted packet counts.

The NomadNet index exposes `/page/espnow.mu` whenever the ESP-NOW interface is
compiled. It is intentionally observational: it reports policy, current state,
traffic, drops, and the selected peer, but has no "scan now" action that could
move a connected station off its infrastructure channel.

Provisioning namespace 102 adds:

| Field | ID | Range/default | Apply |
| --- | ---: | --- | --- |
| ESP-NOW Recovery | 8 | `off` (default), `scan-before-softap` | reboot |
| ESP-NOW Scan Budget | 9 | 5-60 s, default 12 s | live |
| ESP-NOW Rendezvous Channel | 10 | 1-13, board WiFi default | reboot |

Recovery is opt-in for the first hardware-acceptance cycle. Ordinary same-channel
ESP-NOW discovery and transport remain enabled on both RAD revisions.

## Hardware acceptance

Before enabling recovery fleet-wide, exercise Rev1 and Rev2 both associated to
the same infrastructure network and again in the no-router fallback
configuration:

1. Both nodes report the same non-zero WiFi channel and at least one recent
   peer within 12 seconds.
2. Announces and bidirectional Reticulum packets cross ESP-NOW with LoRa
   temporarily isolated.
3. A maximum-size IFAC frame crosses in three fragments.
4. WiFi association, TCP/UDP access, and watchdog health remain stable under
   sustained ESP-NOW traffic.
5. While station WiFi is connected, requesting/checking recovery never changes
   its channel or causes a disconnect.
6. With recovery provisioned and the router absent, a responder on another
   legal channel is found, proven, pinned, and carries bidirectional RNS traffic
   through its LoRa interface before the station retry interval.
7. A wrong IFAC key increments proof failures and ends in the ordinary SoftAP;
   no responder also ends in SoftAP within the scan budget plus loop latency.
8. Different-channel nodes do not claim strict discovery, and
   malformed/incomplete sequences increase drop/timeout metrics without
   resetting either board.
