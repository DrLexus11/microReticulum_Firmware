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
budget/hash/reboot changes. Rev 2 is healthy on UART, its hash was repaired, and
it advertises `4 KB / 8 KB / [16,3,18]`; it has not been reflashed with the last
Rev-1-only response-budget and upload-helper edits. The current Rev 2 UART env
builds successfully, so update it only when a targeted two-board test needs it.

Useful checks:

```sh
~/.local/share/rnode-rns-venv/bin/python tools/rad01/lab_status.py
~/.local/share/rnode-rns-venv/bin/python tools/lxmf/propagation_stress.py \
  ba03aa75f8a136b1b6a74667c755727e --messages 30 --body-bytes 64
```

## Must before the PR is reviewable

1. **Reticchat on iOS.** This is the client whose failure started the work and
   whose background suspension makes store-and-forward necessary. The protocol
   is proven with Columba and Python, but this client remains untested.
2. **Decide the 512 KiB byte-cap contract.** With valid messages capped at 4000
   bytes, `128 × 4000` is below 512 KiB, so the message-count cap always triggers
   first. Either document the byte cap as defense-in-depth, lower it in a test
   build and exercise it, or choose a production value that is independently
   reachable.
3. **Reflash Rev 2 only for the final two-board/LoRa acceptance run.** Build is
   already green. After UART upload, power-cycle out of download mode and run
   `fixhash`; the target now writes the hash and reboots automatically.

## Should in the same PR

4. **Adopt wall time from the first client request.** Announce time is uptime
   because the board has no RTC. Message expiry and peer sync need a stable
   timebase.
5. **Message expiry.** Python expires after 30 days; the firmware currently
   evicts only on capacity. Depends on item 4.
6. **Provisioning controls** for enable/disable, occupancy, limits and manual
   purge. A deployed store is not operator-inspectable today.
7. **NomadNet occupancy page** as the operator-visible surface.
8. **State the no-peering deployment consequence explicitly.** `/offer`
   deliberately declines, protecting the 512 KiB local store from a Linux
   node's backlog, but a RAD cannot hand its backlog to a rooftop blackbox.
9. **Prove the LoRa route.** Watch Rev 2 `tx_calls` during a client sync; an
    increment proves the response crossed RF instead of an IP route.

## Follow-up work

10. **Validate propagation stamps.** The firmware strips the proof but does not
    validate it, so the advertised anti-spam cost is not enforced. Gate this on
    a Python-generated vector; a one-byte mismatch silently rejects real mail.
11. **Make `ACCEPT_APP` effective in microReticulum.** Its resource callback
    returns `void` and `Link.cpp` accepts afterwards, so the firmware currently
    cancels oversize Resources at transfer start instead.
12. **Investigate Resource stalls.** More than ~8 KB reliably times out, and a
    near-8 KB `/get` response also stalled with 128 index entries. The production
    limits now avoid both cases, but windowing/retry/memory behavior deserves a
    separate upstream diagnosis.
13. **Peer sync**, if the blackbox tier ultimately requires it. Resolve items 4
    and 8 first.

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
