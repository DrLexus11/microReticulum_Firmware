# Deck-side BLE peer client

`BLEPeerClientInterface.py` is a Reticulum interface that joins a mesh through
a node's BLE peer service — the role Columba plays. It exists because the phone
is the one part of the chain that cannot be scripted, so every failure it
reports is ambiguous between firmware, protocol and app. A known-correct client
removes two of those three.

It speaks Reticulum BLE peer v2.2, the same wire format as
[`BLEPeerProtocol.h`](../../BLEPeerProtocol.h): a 5-byte `!BHH` fragment header,
a bare 16-byte identity write as the handshake, and a bare `0x00` keepalive
every 15 seconds. It does not pair or bond, because the protocol does not.

## Use

Install it as an RNS external interface, in a config directory of its own so
that nothing but BLE can reach the instance. That isolation is the point: a
result from an instance with other interfaces proves nothing about this one.

```sh
mkdir -p /tmp/rns-ble/interfaces
cp tools/ble/BLEPeerClientInterface.py /tmp/rns-ble/interfaces/
```

```ini
[reticulum]
  enable_transport = No
  share_instance = Yes
  instance_name = bletest          # MUST differ from the default instance,
                                   # or rnsd joins the shared one instead
[interfaces]
  [[OZD BLE Peer]]
    type = BLEPeerClientInterface
    enabled = Yes
    target_address = 40:91:51:9B:2D:D2   # omit to take the first advertiser
```

```sh
rnsd    --config /tmp/rns-ble
rnstatus --config /tmp/rns-ble
rnpath  --config /tmp/rns-ble -w 60 <destination>
```

## What a pass looks like

Against `OZD-ARD-01` bridging to the RAD boards over ESP-NOW:

```
<ba03aa75…> Rev 1 is 2 hops away via <0056bb6a…> on BLEPeerClientInterface
<41fc2ab5…> Rev 2 is 3 hops away via <0056bb6a…> on BLEPeerClientInterface
```

Hop count is the check that matters, not merely that a path was found. Rev 2 at
three hops is `BLE → OZD → ESP-NOW → Rev 1 → LoRa → Rev 2`. If Rev 2 were being
reached back through the deck's own UDP interfaces to the RAD boards it would
read four, and the LoRa hop would be untested while appearing to pass.

Fetching a NomadNet page from Rev 2 over that instance completes the chain.

## Notes

- The node stops advertising while connected, so a stale BlueZ connection makes
  the service unfindable and scans return nothing. `bluetoothctl info <addr>`
  reports `Connected: yes`; disconnect before blaming the firmware.
- The handshake needs the transport identity, which does not exist when
  interfaces are constructed. It is sent on the first pass where Transport has
  one, not at connect time.
- The link comes up at a 23-byte ATT MTU, so packets are fragmented into
  15-byte pieces. Slow, not broken. See `docs/Backlog.md` item 10.
