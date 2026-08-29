# feat(ble): join the mesh as a Reticulum BLE peer

A phone connects over BLE and joins the mesh *through* this node, exactly as it
would over TCP. The node keeps its identity, its radio and its own Reticulum
stack.

This is deliberately not the RNode/KISS-over-BLE model, where a host takes the
radio and the node leaves the network to serve one client. The goal is that a
phone keeps its own Wi-Fi — which SoftAP cannot offer — while still being a peer
rather than a client.

## Verified on hardware

Rev1 as the BLE peer, Columba on Android, Rev2 and a Linux node on LoRa:

- Direct chat, hub groups and group chat
- Messages stuck on the propagation node delivered on reconnect
- Nomadnet sites reachable on **both** Rev1 and Rev2
- Announces propagate phone → BLE → LoRa → mesh; the deck's Nomadnet updates
  the phone's last-seen per announcement
- Columba shows the LoRa antenna symbol for the peer — it is a mesh peer, not a
  modem client

## What it took

Five things had to be right at once; each alone produced a link that looked
healthy and carried nothing.

**Mode.** `MODE_GATEWAY`, not `MODE_ACCESS_POINT`. `Transport::outbound`
explicitly blocks announce broadcasts on AP-mode interfaces. A peer is not an
edge client — the AP choice was the modem model leaking back in.

**Bitrate.** `_bitrate` must be non-zero. `extra_link_proof_timeout()` computes
`(1.0/bitrate)*8*MTU`, so the default 0 gives every link an infinite
establishment deadline: announces flow, links never complete, so no messages and
no pages.

**Stack discipline in BLE callbacks.** `BTC_TASK` has a small fixed stack.
`printf` and `std::string` temporaries overflow it on their own; calling
Reticulum's parse/decrypt/route path from a GATT write is far past it. Both
tripped `Stack canary watchpoint triggered (BTC_TASK)` and rebooted the node
about ten seconds after every connection. Scan results record-and-return;
inbound packets cross to the main loop on a bounded queue.

**The identity handshake.** The peer creates its per-peer Reticulum interface
only once the handshake completes — skip it and the link stays connected and
completely inert. It is an RX write disambiguated by *state*, not length: a
5-byte header plus an 11-byte payload is also 16 bytes.

**Fragment framing.** A single-fragment packet is `START` with `total=1`.
`BleConstants.kt` declares `FRAGMENT_TYPE_LONE = 0x00`, but the client never
emits it, the constant is unused in its Kotlin, and its reassembler drops types
it does not recognise. Sending LONE made the entire outbound direction vanish
while inbound worked perfectly — announces reached the mesh, nothing came back,
and both sides' counters looked fine.

Also: the keepalive is a bare `0x00` every 15 s in both roles, and advertising
must be restarted on disconnect or the node is discoverable exactly once per
boot.

## Retiring the modem service in peer builds

`SetupSerialService()` is compiled out under `BLE_PEER_TRANSPORT`. It was not
merely redundant — its characteristics demand `ENC_MITM`, so a peer connecting
triggered Android pairing that the firmware then refused ("device is not ready
to pair", on a loop). Worse, `bt_host_is_connected()` asked the shared GATT
server, so a peer connecting convinced the firmware a KISS host had attached; it
wrote KISS frames into a `TxCharacteristic` this build never creates and
panicked.

The saving is 28 bytes — the linker was already dropping most of it. The value
is that the service can no longer be served, not the size.

## Diagnostics

Rev1's USB CDC resets the board when a console is attached (`rst:0x15
USB_UART_CHIP_RESET`), so every console read destroyed the session being
diagnosed. The peer diagnostics are exposed over provisioning (ns114) instead,
which reads without resetting: packets in/out, drops, identity writes, MTU, the
last inbound packet's leading bytes, and the raw fragment header last seen on
the wire.

That last field ended the investigation — a day of plausible theories about why
traffic crossed but nothing worked was settled by reading the peer's actual
framing in a single poll.

## Tests

16 tests in `tests/test_ble_peer_protocol.py`, asserted against Columba's own
`BleConstants.kt`. An earlier version gated its cross-checks on an upstream
`BLEFragmentation.py` that never downloaded — 14 bytes of `404: Not Found` — so
those tests skipped silently while appearing to validate. Two tests now pin
findings that contradict the client's declared constants and cannot be left to
comments: START-not-LONE, and the state-gated identity handshake.

## Builds

Verified: `impr-rad01-rev1-portable`, `impr-rad01-rev2`, `lilygo-t3-s3` (BLE, no
`RRC_HUB`) and `rnode-ng-20` (classic Bluetooth). The peer interface is only
compiled where `-DBLE_PEER_TRANSPORT` is set.

An earlier revision of this text claimed `heltec32_v3` built clean. That
environment does not exist in `platformio.ini` -- PlatformIO was rejecting an
unknown environment and the check silently passed. The review caught real
breakage behind that gap; see below.

## Review fixes

Three notes, all accepted, and chasing the third uncovered a regression the
vacuous build check had hidden.

1. **MTU logging in the write path.** `flush()` still printed the negotiated
   MTU directly, which is the unframed-bytes-into-a-KISS-frame failure that
   `checkMTU()`'s own comment warns about. Removed; `update_bt()` logs it.

2. **Header comments contradicted the wire format.** `BLEPeerProtocol.h` still
   asserted the LONE story, including the claim that a lone packet sent as
   START "stalls there" -- the exact inverse of what was measured. Rewritten.

3. **Bluetooth provisioning was gated on `RRC_HUB`.** An earlier commit moved
   the declarations out of that guard but left the registration inside it, so
   any board without the hub silently lost the whole namespace.

   Fixing it surfaced a live regression: the MTU logging had been added to the
   *classic-Bluetooth* `update_bt()`, where `SerialBT` is a `BluetoothSerial`
   with no such members. Every classic-Bluetooth ESP32 board failed to compile,
   and on BLE boards `mtu_log_pending` was set but never consumed, so the line
   never printed either. Moved to the BLE `update_bt()`.

   The namespace is now conditioned on `HAS_BLE` rather than "Bluetooth or
   BLE": every field in it is a BLE concept and a classic board defines none of
   the accessors. Registering it there would mean inventing zero-valued stubs
   for a surface that means nothing on that hardware. Classic-Bluetooth
   provisioning is deliberately left as separate work.

## Known limitations

- Reassembly uses a single buffer, so one peer at a time. Fine for one phone;
  needs per-peer keying before a second connects.
- `_bitrate` is a conservative estimate of usable BLE throughput, not a
  measurement. RNS needs it finite and roughly right.
