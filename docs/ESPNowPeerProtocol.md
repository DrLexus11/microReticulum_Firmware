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

Capability flags: `0x01` LoRa fitted, `0x02` acts as a Reticulum transport,
`0x04` can produce an IFAC admission proof, `0x08` has a route to the mesh that
does not go back out through ESP-NOW. Only `0x04` is authenticated; the rest are
advisory, and `0x08` is used solely to rank candidate parents -- see below.

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

A response that sets `CAP_UPSTREAM` pins the channel and peer for 45 seconds.
One that does not is held as a fallback and the scan continues; it is adopted
only when the budget expires with nothing better -- being reachable badly still
beats not being reachable. A discovery from the pinned MAC, or a complete
Reticulum packet from it that passes the interface's IFAC handling, refreshes
the lease. Merely receiving an ESP-NOW data frame does not. Expiry marks
recovery failed so the established fallback logic can raise SoftAP.

## Choosing a parent, and who is allowed to repeat

Taking the **first** valid response was the original behaviour, and it does not
survive a third node. Two orphans in range of each other both advertise
`CAP_TRANSPORT` and both prove IFAC, so each looks exactly as good as the hub,
and they are as likely to adopt each other as to adopt it. A node cannot route
through a peer that has no route itself.

Reticulum does not correct this afterwards. It learns paths from announces by
**hop count alone**, keeps whichever copy of an announce arrives first, and
never re-evaluates -- it is an overlay for heterogeneous transports, not a radio
routing protocol. Observed directly: OZD-02, one hop from the hub, was recorded
three hops away through a sibling, and Links across that chain would not
establish. So the topology has to be honest to begin with; the router will not
make it so.

`CAP_UPSTREAM` means "I can reach the mesh without going back out through
ESP-NOW" -- true for a LoRa-equipped node, or one associated to infrastructure
Wi-Fi, and never true for the deliberately unconfigured fixtures. Two rules
follow:

1. **Prefer a peer that sets it**, and take one that does not only as a last
   resort.
2. **A node that does not set it stops relaying while it has a parent.** Every
   announce it repeats reaches the hub one hop longer than the copy the hub
   already heard directly, and costs a duplicate flood to produce it. The
   moment the parent is lost this reverses -- it is then the only thing keeping
   its neighbours reachable -- so the policy is re-applied on both transitions.
   Nodes that are themselves a way out are never touched by it.

### Where this sits against the literature

The problem is ordinary and well studied; the useful part is which idea is
worth borrowing at this scale.

| Protocol | Mechanism | Relation to the above |
| --- | --- | --- |
| **RPL** | *Rank* -- distance to root, must strictly improve when choosing a parent | `CAP_UPSTREAM` is rank reduced to one bit |
| **batman-adv** | *TQ* from OGM reception ratios; best next hop by link quality, re-evaluated continuously | What we would need if hop count alone stopped being good enough |
| **Babel** | Distance-vector with a *feasibility condition*; loop-free without full path knowledge | The rule to adopt if re-parenting ever becomes dynamic |
| **OLSR** | *MPR* -- only a covering subset of neighbours rebroadcasts | Rule 2 is the degenerate case: the covering set of a hub-and-spoke is the hub |
| **Meshtastic** | SNR-weighted rebroadcast delay, cancelled on overhearing | The next step if multi-hop ESP-NOW is ever wanted |

The common property is that **none of them let first-arrival decide.** Each
either measures the link, ranks candidates by distance to a root, or elects who
may repeat. One bit buys the third of those, which is the one this topology
needs; the others stay available if the shape of the network changes.

## Reliability and latency: what was measured

Bench measurements, 2026-09-03, deck to node, six or more Reticulum link
setups plus one page fetch each:

| Target | Path | Link setup (median) | Page fetch (median) | Failures |
| --- | --- | ---: | ---: | ---: |
| Rev 1 | deck -> UDP | **0.37 s** | 0.83 s | **0 / 10** |
| OZD-02 | deck -> UDP -> ESP-NOW | **1.45 s** | 3.4 s | **3 / 6 and worse** |

Two separate problems sit behind those numbers.

### Wi-Fi power save, and why we cannot simply turn it off

ICMP to Rev 1 measures min 2.7 ms, avg 53 ms, **max 183 ms**. That distribution
is not a network; it is a station sleeping between DTIM beacons and answering on
the next wake. A Reticulum link handshake is several round trips and pays the
toll on each, which is most of the 0.37 s setup seen on a quiet LAN.

For ESP-NOW it is worse than latency. ESP-NOW has no buffering for a sleeping
peer: a unicast frame is retried by the 802.11 MAC until the peer wakes, but a
**broadcast that lands while the radio is down is simply gone**.

The obvious fix is not available. Wi-Fi and Bluetooth share one 2.4 GHz radio
on the ESP32 and their coexistence scheduler is built on the modem-sleep slices,
so ESP-IDF does not warn -- it aborts at Wi-Fi init:

```
E wifi: Error! Should enable WiFi modem sleep when both WiFi and Bluetooth
        are enabled!!!!!!
abort() was called
```

Verified the hard way: both Rev 1 and the OZD fixture went into a boot loop.
`WIFI_NO_POWER_SAVE` in `Remote.h` therefore compiles in only for a node with
no Bluetooth, which today is none of ours. **Choosing between BLE and a
low-latency ESP-NOW mesh on one ESP32 is a product decision, not a tuning
flag.** A hub that dropped Bluetooth would get both lower latency and fewer
lost ESP-NOW frames; the cost is the BLE peer transport on that board.

### The downstream direction is unacknowledged

A node pinned to a parent unicasts to it, so upstream traffic is ACKed and
retried in hardware. The hub is not pinned to anything and has several peers,
so **everything it sends downstream is broadcast** -- unacknowledged, one
attempt, and the strictly sequential reassembler discards a whole packet when
any fragment of it is lost. Announces from a leaf therefore arrive reliably
while the hub's replies do not, which is exactly the observed shape: paths stay
fresh and links fail to establish.

Naive per-peer unicast fan-out was tried and **made it worse** (6/8 failures
against 3/6). The sender is strictly one frame in flight, serviced once per
firmware loop, so addressing peers in turn multiplies head-of-line blocking:
one slow or absent peer stalls the queue for every other. It was reverted. Any
future attempt needs the send path to stop being serial first, or to skip peers
that are failing, rather than simply addressing more of them.

Not yet isolated: OZD-02 performs markedly worse than OZD-01 on the same
channel, at the same hop count, from the same parent -- and neither the relay
policy (tested by neutralising it) nor the fan-out explains it. OZD-02 is the
unit attached by USB to the machine running the tests, which is a well-known
2.4 GHz desense arrangement, so the RF environment is the first thing to rule
out before more code is written.

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

The dedicated `ozdisan-esp32-espnow` acceptance target is the one exception to
the default-off policy. It has no LoRa modem or station credentials, so it
defaults to `scan-before-softap` in order to exercise recovery as its primary
mesh ingress. See [OzdisanESPNowFixture.md](OzdisanESPNowFixture.md).

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
