# ESP32 Propagation Node — Remaining Work

Current handoff for `feature/esp32-lxmf-prop-nodes`, updated 2026-08-24 after
the hardware hardening run. Background and evidence are in
`docs/Messaging.md`, especially §§7–10.

## Current state

The propagation node is working on hardware and interoperates with stock Python
LXMF and Columba on Android. Packet and Resource ingest, offline retrieval,
multi-message truncation, purge, persistence and oldest-first count eviction
have been exercised. On the final 4096-byte response build, the deterministic
129-message run returned exactly the newest 128 messages in `13 × 9, 11, 0`
rounds, with no retries, missing, unexpected or duplicate bodies.

Rev 1 is the primary development board and is flashed with the final response
budget/hash/reboot changes. Rev 2 is also on the final UART build: target and
running hashes match, radio state is on, and it advertises
`4 KB / 8 KB / [16,3,18]`.

The final two-board acceptance is conclusive. Columba attached to Rev 1 over TCP
retrieved the exact labelled message from Rev 2; Rev 2 logged `/get` list,
serve and purge, explicit radio sends, and `tx_calls` increasing from 7 to 13.
Both boards remained healthy afterward. The byte-cap contract is also settled:
128 messages is the primary production cap and 512 KiB is defense-in-depth,
with an invariant test preventing those semantics from drifting silently.

Useful checks:

```sh
~/.local/share/rnode-rns-venv/bin/python tools/rad01/lab_status.py
~/.local/share/rnode-rns-venv/bin/python tools/lxmf/propagation_stress.py \
  ba03aa75f8a136b1b6a74667c755727e --messages 30 --body-bytes 64
```

## PR acceptance status

**Complete.** Packet and Resource ingest, Python/Columba interoperability,
multi-message batching, maximum-count eviction, final-build full-store sync,
UART hash recovery, and a real Rev 1↔Rev 2 LoRa retrieval are all verified.
Reticchat is unavailable; it remains useful client-matrix coverage but is not a
blocker after exact-body delivery through stock Columba over the RF path.

## Follow-up work

1. **Reticchat on iOS**, when it becomes available. Repeat the offline-store and
   reopen/sync test as additional client coverage.
2. **Adopt wall time from the first client request.** Announce time is uptime
   because the board has no RTC. Message expiry and peer sync need a stable
   timebase.
3. **Message expiry.** Python expires after 30 days; the firmware currently
   evicts only on capacity. Depends on item 2.
4. **Provisioning controls** for enable/disable, occupancy, limits and manual
   purge. A deployed store is not operator-inspectable today.
5. **NomadNet occupancy page** as the operator-visible surface.
6. **Validate propagation stamps.** The firmware strips the proof but does not
    validate it, so the advertised anti-spam cost is not enforced. Gate this on
    a Python-generated vector; a one-byte mismatch silently rejects real mail.
7. **Make `ACCEPT_APP` effective in microReticulum.** Its resource callback
    returns `void` and `Link.cpp` accepts afterwards, so the firmware currently
    cancels oversize Resources at transfer start instead.
8. **Investigate Resource stalls.** More than ~8 KB reliably times out, and a
    near-8 KB `/get` response also stalled with 128 index entries. The production
    limits now avoid both cases, but windowing/retry/memory behavior deserves a
    separate upstream diagnosis.
9. **Peer sync**, if the blackbox tier ultimately requires it. Resolve item 2
   first. A RAD deliberately declines `/offer`, so it cannot currently hand its
   local backlog to a rooftop blackbox.

## Lab facts worth preserving

- Rev 1: native USB, `/dev/ttyACM1`, MAC suffix `C7:A4`, `192.168.1.54`, PN
  `ba03aa75f8a136b1b6a74667c755727e`.
- Rev 2: CP2102N UART bridge, `/dev/ttyUSB0`, `192.168.1.88`, PN
  `41fc2ab5e88d0b355d3c35fa60f4a22e`.
- `/dev/ttyACM0` is the Steam Deck controller, not a RAD.
- Opening Rev 1 serial resets/re-enumerates it; use network checks first.
- Only one process may hold a serial port.
- Rev 2 UART upload uses `--after=no_reset`; `fixhash` is mandatory after the
  board is running or `hw_ready` remains zero and RF never starts.
