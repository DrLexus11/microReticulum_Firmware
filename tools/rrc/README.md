# RRC interoperability probe

`probe.py` is the repeatable, headless acceptance client for the embedded RRC
v1 hub. It uses two persistent Reticulum identities and NomadNet's CBOR codec,
without depending on NomadNet UI state.

The default run:

1. resolves the supplied `rrc.hub` destination;
2. establishes and identifies two encrypted Links;
3. validates `WELCOME` and its advertised limits;
4. joins both identities to one ephemeral room;
5. verifies exact messages and authenticated attribution in both directions;
6. abruptly disconnects client B and verifies membership cleanup at client A;
7. reconnects B, rejoins and verifies another exact message; and
8. parts both clients and tears down their Links.

Run it with the same virtual environment used by the Deck RNS daemon:

```console
~/.local/share/rnode-rns-venv/bin/python tools/rrc/probe.py \
  d36d1371772fca94fb6dc2522d1c4254 \
  --expect-hub-name "IMPR-RAD RRC"
```

Use `--rns-configdir` for an isolated Reticulum fixture, `--state-dir` to select
different persistent test identities, and `--json` for machine-readable output.
The default identities live under ignored `state/rrc-probe/`.

The probe retries both Link establishment and `HELLO`, since either can be lost
on a real low-bandwidth path. It answers hub `PING` packets with `PONG` while an
acceptance run is active.

## Isolated two-board path

The normal Deck Reticulum instance has direct interfaces to both boards. Do not
use it to claim a Rev2-to-Rev1 LoRa test, because it can select the one-hop Rev1
path. Create a separate Reticulum configuration containing only:

```ini
[reticulum]
  enable_transport = No
  share_instance = No
  instance_control_port = 37540
  panic_on_interface_error = No

[interfaces]
  [[Rev2 Direct TCP]]
    type = TCPClientInterface
    enabled = Yes
    target_host = 192.168.1.88
    target_port = 4242
    kiss_framing = No
```

If that file is `/tmp/rrc-via-rev2/config`, the mixed automated run is:

```console
~/.local/share/rnode-rns-venv/bin/python tools/rrc/probe.py \
  d36d1371772fca94fb6dc2522d1c4254 \
  --expect-hub-name "IMPR-RAD RRC" \
  --rns-configdir /tmp/rrc-via-rev2 \
  --state-dir state/rrc-probe-via-rev2 \
  --message-prefix REV2-LORA-REV1 \
  --json
```

## Stock NomadNet and Eridanus acceptance

Run a separate stock NomadNet profile through the isolated Rev2 configuration:

```console
nomadnet --config /tmp/nomadnet-via-rev2 \
  --rnsconfig /tmp/rrc-via-rev2 --textui
```

In NomadNet, open **Channels**, add Rev1's hub
`d36d1371772fca94fb6dc2522d1c4254`, connect, and join a uniquely named room.
On Android, attach Columba to Rev1 TCP at `192.168.1.54:4242`, enable its Shared
Instance, and join the same hub and room from Eridanus. Exchange unique exact
markers in both directions, then terminate and reconnect one client. Run
`tools/rad01/lab_status.py` afterward.

Current Eridanus automatically emits `/who <room>` after joining. Reference
`rrcd` consumes that extension and answers privately. Until the deferred minimal
`/who` handler is added, the embedded MVP hub relays it as ordinary room text;
this is known and does not indicate that the RRC Link or room join failed.
