# ESP32 Propagation Node — Remaining Work

What is left to take `feature/esp32-lxmf-prop-nodes` to a PR that settles the
feature. Written 2026-08-24 at the end of the session, to be picked up cold.

Background is in `docs/Messaging.md`: §7 what was built, §8 verification against
a real client, §9 a flashing hazard, §10 the hardening pass.

---

## Where it stands

**Working and verified on hardware.** An ESP32 node announces itself as an LXMF
propagation node, accepts a message for an absent recipient over both the packet
and Resource paths, stores it across the recipient being offline, and serves it
on demand. Verified end to end against **Columba on Android** — stock client,
unmodified protocol — and against the Python LXMF library.

Enabled by default for both RAD environments. Flash 84.9% (Rev 1), 85.6%
(Rev 2). 21 tests pass (`python -m unittest discover tests` under the RNS venv).

### Hardware state at end of session

| | |
| --- | --- |
| **Rev 1** | Current build. Advertising `transfer=4 KB, sync=8 KB`. Healthy. |
| **Rev 2** | Flashed at end of session; **needs `fixhash` after its next power-on** — see below. |

Rev 2 was flashed from an env that uploads with `--after=no_reset`, so the
firmware hash could not be written. Until it is, `hw_ready` stays 0 and **the
radio will not start** — the node joins over WiFi and serves pages while being
deaf on RF. The build now detects this and prints the fix; the sequence is:

```
# power-cycle Rev 2 out of download mode first, then:
pio run -e impr-rad01-rev2-uart -t fixhash --upload-port /dev/ttyUSB0
# then reboot it so device_init() re-validates
```

---

## Must, before the PR is reviewable

1. **Store-full behaviour.** Fill to `LXMF_PN_MAX_MESSAGES` (128) and
   `LXMF_PN_MAX_BYTES` (512 KB), confirm eviction is sane and that `/offer` and
   `/get` stay correct with a full store. Entirely untested today.
2. **Multi-message sync.** Several messages for one recipient in a single
   `/get`, which exercises the transfer-limit truncation path in
   `lxmf_message_get_request()`. Also untested, and now more likely to matter
   with an 8 KB sync limit forcing more round trips.
3. **Reticchat on iOS.** The client whose failure started this work, and the one
   whose backgrounding makes store-and-forward necessary at all. Two messages
   are already waiting for it on the Deck's `lxmd`
   (`5e87be5e08495b9d94b96442b9313d8a`).
4. **Reflash Rev 2** to the current build and confirm it advertises `4, 8` — it
   is still on the older build advertising `8, 64`.

## Should, same PR

5. **Adopt wall time from the first client request.** The announce timebase is
   currently uptime, because the board has no RTC. Nothing observed depends on
   it, but message expiry cannot be implemented without it, and peer sync would
   care. This blocks item 6.
6. **Message expiry.** Python expires at `MESSAGE_EXPIRY = 30 days`; we only
   evict on capacity. Depends on item 5.
7. **Provisioning namespace** — enable/disable, store count and bytes, limits,
   manual purge. Today a deployed node's store cannot be inspected or bounded
   without a reflash.
8. **NomadNet page** showing store occupancy, as the operator-visible surface.
9. **State the no-peering decision explicitly** in `docs/Messaging.md`, with its
   consequence: a RAD cannot hand its backlog to a rooftop blackbox, which is
   the deployment model that document describes. Currently a deliberate v1
   choice recorded only in a code comment.

## Follow-up, separate PRs

10. **Validate propagation stamps.** We strip the proof-of-work but never check
    it, so the anti-spam cost we advertise is not enforced and a sender can
    append 32 arbitrary bytes. Cost is one 1000-round HKDF workblock (256 KB,
    streamable) plus one hash per message — affordable on an S3. **Gate this on
    a Python-generated test vector**: a one-byte divergence silently rejects
    real messages, which is the failure mode this project keeps losing days to.
11. **Make `ACCEPT_APP` mean something** in microReticulum (our own fork). Its
    resource callback returns `void` and `Link.cpp` accepts unconditionally
    afterwards, so the strategy that exists in Python for refusing a Resource
    cannot refuse anything here. We work around it by cancelling on transfer
    start; the proper fix is upstream.
12. **Peer sync**, if the blackbox tier needs it. See item 9.

---

## Open questions worth answering

**Why do transfers stall above ~8 KB?** Measured on Rev 1: 8 KB completes in
2.0 s, 16 KB and 32 KB stall and time out after ~104 s. The advertised limits
were lowered to match, so nothing is broken — but this ceiling is lower than the
hardware ought to manage, and the ~104 s figure looks like a fixed timeout being
hit rather than a size limit. Suspects: window sizing, retry behaviour, or
memory during reassembly.

**Did the Columba retrieval actually cross the LoRa hop?** Strongly indicated —
Columba reaches Rev 2 only through Rev 1, whose shortest route is one radio hop
versus two via the Deck — but not proven, because the Deck cannot observe Rev 1's
routing decision. **To settle it:** watch Rev 2's `tx_calls` during a sync. That
counter increments only on LoRa transmit, so if it moves while `/get` is being
served, the response went out over the radio.

---

## Things that cost time today, so they do not cost it twice

- **Opening a board's serial port reboots it**, and on Rev 1's USB-CDC the port
  re-enumerates so the old handle goes dead. Two wrong conclusions today came
  from captures that began after the interesting lines had printed. Attach
  first, then reboot.
- **Only one process may hold a serial port.** A second reader kills both.
- **Rev 1's logs are KISS-framed**, so plain-text capture from it can show
  nothing while the node is perfectly healthy. Check liveness over the network
  (`tools/lxmf/fetch_page.py`) rather than over serial.
- **`-292` is the initial value of `noise_floor` and `last_rssi`**
  (`Config.h`), not a fault. It means "no samples yet".
- **`startRadio()` will explain itself** if asked over KISS — send
  `C0 06 01 C0` and it prints `startRadio OK` or `BLOCKED locked=? hwr=?`. This
  is the fastest way to find out why a radio is not running, and would have
  saved most of an afternoon.
