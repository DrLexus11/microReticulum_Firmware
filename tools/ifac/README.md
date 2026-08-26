# IFAC and secure-node provisioning

`provision.py` configures the LoRa, TCP and UDP interface access codes compiled
into a board and the fully private secure-node posture over its **physical**
KISS serial connection. Run it with the repository's RNS virtualenv so
`pyserial` and MsgPack are available:

```sh
~/.local/share/rnode-rns-venv/bin/python tools/ifac/provision.py \
  --port /dev/ttyACM1 schema --interface all
```

Passphrases are read from terminal prompts. They are never accepted on the
command line, so they do not appear in shell history or process listings. The
tool refuses schemas that do not mark Passphrase as `SECRET` and verifies that
neither draft nor committed state echoes it.

## Interface access control

Each interface has independent credentials and can be protected without
changing the others:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 \
  enable --interface lora --network "Backbone"
python tools/ifac/provision.py --port /dev/ttyACM1 \
  enable --interface tcp --network "Agency clients"
python tools/ifac/provision.py --port /dev/ttyACM1 \
  enable --interface udp --network "Operations LAN"
```

The firmware uses an 8-byte Python-compatible IFAC on LoRa and 16-byte IFACs
on TCP and UDP. Enabled but incomplete/corrupt configurations fail closed
instead of silently opening the affected interface. All fields are
reboot-required: configure every peer before rebooting any of them.

The old LoRa-only command remains compatible because `--interface` defaults to
`lora`:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 enable --network "Backbone"
```

Disable one interface with:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 \
  disable --interface tcp --clear-credentials
```

Individual disable operations are rejected while Secure Node is enabled,
because secure mode deliberately forces every compiled RNS interface to remain
protected or fail-closed. Use the atomic `open` operation below when returning
a secure node to an open posture.

## Fully private secure-node posture

The `secure` operation discovers the IFAC namespaces advertised by the board
and stages one transaction containing:

- every compiled RNS interface IFAC enabled with complete credentials;
- remote management enabled with the supplied administrator identity hash;
- the administrator allow-list; and
- the secure-node switch that disables WiFi KISS on TCP 7633, the optional
  KISS WebSocket endpoint, and Bluetooth KISS after reboot.

Physical USB/UART KISS is deliberately unaffected and remains the recovery
path. The administrator is a 16-byte Reticulum identity hash, written as 32
hexadecimal characters.

Use independent network names and prompted passphrases:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 secure \
  --lora-network "Agency backbone" \
  --tcp-network "Agency clients" \
  --udp-network "Agency LAN" \
  --administrator 0123456789abcdef0123456789abcdef
```

Options for interfaces not compiled into the connected board are ignored by
the secure transaction and do not require values. If an installation
deliberately uses one name for every available interface, `--network` provides
the common fallback. `--shared-passphrase` prompts only once and reuses that
secret explicitly:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 secure \
  --network "Agency mesh" --shared-passphrase \
  --administrator 0123456789abcdef0123456789abcdef
```

Independent credentials are preferred when the node bridges distinct trust
domains. A public QuakeMesh resident node should not enable this posture until
its actual phone client is confirmed to support TCP IFAC.

After staging all peers, reboot them explicitly:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 --boot-wait 0 reboot
python tools/ifac/provision.py --port /dev/ttyUSB0 --boot-wait 0 reboot
```

## Physical recovery and factory reset

To return a reachable unit to the open posture, connect its physical USB/UART
and disable secure mode plus every advertised IFAC in one staged transition:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 \
  open --clear-credentials
python tools/ifac/provision.py --port /dev/ttyACM1 --boot-wait 0 reboot
```

Factory reset restores the same open defaults. Never perform the first secure
transition on a remotely mounted node until the physical recovery port and the
administrator identity have both been exercised on the bench.

Commit responses include persistent-storage failures. The tool treats one as a
failed operation and does not claim that the secure posture was stored; correct
the storage fault and repeat the operation before rebooting.

Opening Rev1's native USB serial port resets that board. Rev2's external UART
bridge normally does not. A Rev2 UART firmware upload still requires the
post-flash firmware-hash write described in `docs/PropagationNodeTODO.md`;
secure mode does not change that recovery requirement.

## Lab diagnostics

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 radio
python tools/ifac/provision.py --port /dev/ttyUSB0 addresses
python tools/ifac/provision.py --port /dev/ttyACM1 mode host
python tools/ifac/provision.py --port /dev/ttyACM1 mode tnc
```

`mode host` temporarily turns a board into a conventional host-driven RNode;
`mode tnc` restores this firmware's autonomous transport-node role. Both are
reboot-required.

Provisioned passphrases are not encrypted at rest in LittleFS. `SECRET` means
write-only over the provisioning protocol, not resistance to physical
extraction from a captured board.
