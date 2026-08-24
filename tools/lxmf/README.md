# LXMF propagation node test tools

Host-side scripts for exercising a RAD acting as an LXMF propagation node.
They speak the real protocol through the Python LXMF library, which is the
point: node-to-node testing proves nothing about bit-compatibility with the
clients people actually run.

Run them under the RNS virtualenv:

    ~/.local/share/rnode-rns-venv/bin/python tools/lxmf/<script> ...

| Script | What it does |
| --- | --- |
| `interop.py <pn_hash>` | Full round trip: sender A pushes a message for recipient B while B is offline, then B syncs and reads it. Set `LXBODY_PREFIX` to a long string to force the Resource path instead of the packet path. |
| `send_to_columba.py <pn_hash> <dest_hash> [body]` | Push one message for a real client (a phone) into a node's store and stop, so the client can sync it later. |
| `oversize.py <pn_hash> [bytes]` | Push a Resource of a given size at the propagation destination. Used to verify the size guard refuses oversize transfers, and to measure the board's real ceiling. |
| `watch_announces.py` | Print `lxmf.delivery` and `lxmf.propagation` announces as they arrive. How to learn a phone's address without reading a hash off its screen. |
| `fetch_page.py <hash> [path]` | Fetch a NomadNet page from a node, for checking a board is alive over the network rather than over serial. |

## Gotchas learned the hard way

- **Opening a board's serial port reboots it.** Several "the radio is dead"
  conclusions during development were really a capture that started after the
  interesting lines had already printed. Attach first, then reboot.
- **Only one process may hold a serial port.** A second reader gets
  "device reports readiness to read but returned no data", and both then
  produce nothing.
- **`recall_app_data()` returns cached announce data**, which can be minutes or
  hours stale. To see a node's *current* advertised limits, wait for a fresh
  announce with `watch_announces.py` rather than trusting the cache.
- **A fresh `Reticulum()` client does not inherit the daemon's path table.**
  Call `RNS.Transport.request_path()` and wait before concluding a destination
  is unreachable.
