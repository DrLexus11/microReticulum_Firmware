# RAD-01 Deck-lab tools

`lab_status.py` checks the two development boards without opening either serial
port. That distinction matters: Rev 1 resets and re-enumerates when native USB
is opened, and Rev 2's external bridge can affect reset/boot depending on the J3
wiring.

Run it under the RNS virtual environment so propagation announce payloads can be
decoded and checked against the LXMF reference library:

    ~/.local/share/rnode-rns-venv/bin/python tools/rad01/lab_status.py

The expected lab mapping is:

| Board | Stable serial path | IP | Propagation destination |
| --- | --- | --- | --- |
| Rev 1 | Espressif USB-JTAG/serial `80:B5:4E:F4:C7:A4` | `192.168.1.54` | `ba03aa75f8a136b1b6a74667c755727e` |
| Rev 2 | CP2102N `9a255dc4f508f11185e86bc3813d7cb7` | `192.168.1.88` | `41fc2ab5e88d0b355d3c35fa60f4a22e` |

The command fails if a serial device is missing or busy, a board does not answer
ICMP/TCP, or recalled propagation app data does not advertise the current
`4 KB / 8 KB / [16,3,18]` contract.

LXMF packet, Resource, multi-message and capacity exercises live beside the
existing protocol tools in `tools/lxmf/`. See its README before running a test
that intentionally fills and evicts the propagation store.
