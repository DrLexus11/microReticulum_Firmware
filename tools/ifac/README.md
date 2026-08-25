# LoRa IFAC provisioning

`provision.py` configures the firmware's LoRa access code directly over its
local KISS serial connection. Run it with the repository's RNS virtualenv so
`pyserial` and MsgPack are available:

```sh
~/.local/share/rnode-rns-venv/bin/python tools/ifac/provision.py \
  --port /dev/ttyACM1 schema
```

Passphrases are read with a terminal prompt. They are not accepted on the
command line, so they do not appear in shell history or process listings. The
tool also refuses to provision a firmware schema that does not mark Passphrase
as `SECRET`, and verifies that neither draft nor committed state echoes it.

## Enable a mesh

Commit the same network name and passphrase to every peer before rebooting any
of them:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 enable --network "Mesh name"
python tools/ifac/provision.py --port /dev/ttyUSB0 enable --network "Mesh name"

python tools/ifac/provision.py --port /dev/ttyACM1 --boot-wait 0 reboot
python tools/ifac/provision.py --port /dev/ttyUSB0 --boot-wait 0 reboot
```

The firmware uses an 8-byte Python-compatible IFAC on LoRa. TCP and UDP local
attachment remain open. An enabled but incomplete/corrupt configuration is
fail-closed rather than silently falling back to open radio operation.

## Return to an open mesh

Disable every peer, then reboot them. `--clear-credentials` also removes the
stored network name and passphrase; without it, they remain available for a
later re-enable but are still omitted from GetState responses.

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 disable --clear-credentials
python tools/ifac/provision.py --port /dev/ttyUSB0 disable --clear-credentials
```

## Lab diagnostics

The following commands were used by the Python-RNS hardware fixture:

```sh
python tools/ifac/provision.py --port /dev/ttyACM1 radio
python tools/ifac/provision.py --port /dev/ttyUSB0 addresses
python tools/ifac/provision.py --port /dev/ttyACM1 mode host
python tools/ifac/provision.py --port /dev/ttyACM1 mode tnc
```

`mode host` temporarily turns a board into a conventional host-driven RNode;
`mode tnc` restores this firmware's autonomous transport-node role. Both are
reboot-required. Opening Rev 1's native USB serial port itself resets that
board, while Rev 2's external UART bridge normally does not.

The provisioned passphrase is not encrypted at rest in LittleFS. `SECRET`
means write-only over the provisioning protocol, not resistance to physical
extraction from a captured board.
