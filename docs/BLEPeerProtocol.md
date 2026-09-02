# The node as a BLE peer

A phone connects over BLE and joins the mesh *through* this node, exactly as it
would over TCP. The node keeps its identity, its radio and its own Reticulum
stack; BLE is simply another interface.

This is deliberately not the RNode/KISS-over-BLE model, in which a host takes
the radio and the node leaves the network to serve one client. The goal is that
a phone keeps its own Wi-Fi — which SoftAP cannot offer — while still being a
peer rather than a client.

Implementation: [`BLEPeerInterface.h`](../BLEPeerInterface.h), wire constants in
[`BLEPeerProtocol.h`](../BLEPeerProtocol.h), pinned by
[`tests/test_ble_peer_protocol.py`](../tests/test_ble_peer_protocol.py).
Built only where `-DBLE_PEER_TRANSPORT` is set. Rev1/Rev2 use the proven
Bluedroid peer backend; `ozdisan-esp32-espnow` uses NimBLE to fit BLE, Wi-Fi,
ESP-NOW and Reticulum in an original ESP32 with no PSRAM. Neither backend asks
the operating system to pair or bond.

## Wire format

Protocol v2.2. Service `37145b00-442d-4a94-917f-8f42c5da28e3`, with RX (`…e5`,
central→peripheral writes), TX (`…e4`, peripheral→central notifications) and
Identity (`…e6`, read-only, 16 bytes).

Each fragment carries a 5-byte header — `struct.pack("!BHH", type, seq, total)`.

| Frame | Encoding |
|---|---|
| Single-fragment packet | `01 0000 0001` — START with `total=1` |
| Multi-fragment packet | START, CONTINUE…, END |
| Keepalive | one `0x00` byte, every 15 s, both roles |
| Identity handshake | bare 16 bytes written to the peer's **RX** |

### A single-fragment packet is START, not LONE

`BleConstants.kt` declares `FRAGMENT_TYPE_LONE = 0x00`. **The client never emits
it**, the constant is unused anywhere in its Kotlin, and its reassembler
discards types it does not recognise.

This was measured, not inferred: every single-fragment packet from the client
arrives as `01 0000 0001`. Sending `0x00` instead makes the entire outbound
direction vanish while inbound continues to work perfectly — announces reach the
mesh, nothing ever comes back, and both sides' counters look healthy.

We accept LONE on receive, defensively. We never send it.

### The identity handshake gates everything

The peer creates its per-peer Reticulum interface *only* once the handshake
completes. Skip it and the link stays connected, healthy, and completely inert:
packets cross the wire and have nowhere to be delivered.

- As **peripheral**: a 16-byte RX write is the handshake when — and only when —
  no identity is yet recorded for that peer. Length alone is ambiguous, because
  a 5-byte header plus an 11-byte payload is also 16 bytes.
- As **central**: read the peer's Identity characteristic, then write our own
  16 bytes to their RX. Identity travels on RX only; it is never a TX
  notification, because the client feeds every notification straight to its
  defragmenter.

## Roles

The protocol breaks symmetry by address: the lower address connects, the higher
waits as a peripheral. Advertising alone is therefore not enough — two
peripheral-only nodes sit advertising at each other forever — so this node also
scans and initiates when it holds the lower address.

Addresses rotate, so connection failures suppress only the failed over-the-air
address for a bounded interval. A scan burst is allowed to finish and the
strongest eligible peer is selected; dialing the first advertisement forever
lets one unreachable RAD starve a nearby phone. Peers are tracked by Reticulum
identity rather than address after the handshake.

## Constraints this code exists to satisfy

**Interface fields RNS divides by.** `_bitrate` must be non-zero:
`Transport::extra_link_proof_timeout()` computes `(1.0/bitrate)*8*MTU`, so the
default 0 gives every link an infinite establishment deadline. Links never
complete — no messages, no pages — while announces still flow.

**Mode.** `MODE_GATEWAY`, not `MODE_ACCESS_POINT`. `Transport::outbound`
explicitly blocks announce broadcasts on AP-mode interfaces ("Blocking announce
broadcast … due to AP mode"). A peer is not an edge client.

**BLE callbacks have a small fixed stack.** Bluedroid callbacks run on
`BTC_TASK`; NimBLE likewise invokes callbacks on its host task. Doing real work
there can reboot or deadlock the node. Scan results record-and-return, connection
completion is deferred, and inbound packets go on a bounded queue that the main
loop drains.

**One inbound link owns the interface.** Identity and fragment reassembly are
connection state. They are reset when that link disconnects; a concurrent
second inbound connection is rejected rather than allowing its handshake to be
parsed as data for the first peer.

**Advertising must be restarted.** The controller stops advertising on connect.
Nothing else in a peer build restarts it, so without an explicit restart on
disconnect the node is discoverable exactly once per boot: the first connection
works and no later one ever does.

## Diagnosing it

Rev1's USB CDC **resets the board when a console is attached** (`rst:0x15
USB_UART_CHIP_RESET`) — only `dtr=False, rts=True` yields output, and it reboots
the node. Every console read destroys the session being observed.

The peer diagnostics are therefore exposed over provisioning (ns114), which
reads without resetting: packets in/out, fragments dropped, identity writes,
negotiated MTU, the last inbound packet's size and leading bytes, and the raw
fragment header last seen on the wire. Reading the peer's actual framing is what
identified the LONE mistake after a day of plausible theories.
