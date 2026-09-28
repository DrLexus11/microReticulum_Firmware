# What the original fork has that we do not -- reviewed 2026-09-28

**Decided by the operator, 2026-09-28:** OCP and init order -> PR E; radio
reset at boot -> PR E if it applies; ISR/SPI fix -> check, disregard if ours
covers it; RX-state refresh -> as proposed; IDF 5 upgrade -> backlog, after the
unit tests are consolidated to catch regressions; native builds -> Vox phase;
RAK/SD -> disregarded. Scheduled in `TAKDeliveryPlan.md`.

**Checked 2026-09-28:**
- *ISR/SPI mutex fix* -- **already ours**, disregarded. All three drivers
  (sx126x, sx127x, sx128x) defer the DIO0 work out of the interrupt with a
  pending flag handled in the loop, the same pattern as 0124fd8.
- *Radio reset at boot* -- **already ours**: `RNode_Firmware.ino` resets the
  radio before `preInit()` (three paths). The "deaf after flash" symptom is not
  the radio but the firmware signature (`fw_signature_validated` false leaves
  `hw_ready` 0 and the loop stops the radio; cure: `pio run -t fixhash`, see
  memory). What 127c698 adds for sx126x is `isResponding()`, a single-shot
  health probe for the native daemon's runtime watchdog -- **moved to the Vox
  phase** with the native builds.

Sources: `attermann/microReticulum_Firmware` (fetched 2026-09-28) and
`attermann/microReticulum`, the library.

**Neither master has moved since we forked.** The firmware's `master` ends at
592c826 (2026-07-23), our fork point; the library's at 40fa628 (2026-07-20),
with 26 commits of ours on top of it. What we lack is on side branches, and
most of their commit count is history rebuilt onto Mark Qvist's upstream for
pull requests there, already in our tree by another route. Compared by content,
these are the changes that matter to our hardware:

| Change | Branch | For us | Proposed |
| --- | --- | --- | --- |
| **SX1262 over-current limit 140 mA** (06511a2) | rak13302 / rak4631 / sx1262 | Our driver writes the board's `OCP_TUNED` after the PA is configured -- 0x28, about 100 mA, on both RAD boards -- replacing the chip's own 140 mA high-power default. Near 21-22 dBm the SX1262 draws ~110-140 mA, so Rev 2 may be current-limited below the power it is set to. Raising the limit only lets it reach its setting; it does not raise the setting. | **PR E**, measured: RSSI at a fixed receiver and supply current, before and after. Range is what Outdoor Test 1 measures. |
| SX1262 init order: standby RC, regulator, TCXO, clear errors, then calibrate (same commit) | same | We already select DC-DC (found independently). Rev 2 has no TCXO, so the reordering matters less here. | Take with the OCP change if the diff applies cleanly. |
| **Radio reset at boot** (127c698, sx126x part) | native_watchdog | We know a freshly flashed board can come up deaf on LoRa until power-cycled (bench note). A reset of the radio at boot is the standard cure. | **PR E**, small; verify with a flash-then-listen test. |
| sx126x: refresh RX state per packet, DIO0 diagnostics (c73dca7) | sx1262 | Possible RX robustness; our sx126x has diverged a lot. | Read against ours after the two above; take only what applies. |
| ISR/SPI mutex crash fix in modem drivers (0124fd8) | isr_race | A crash in the radio ISR path is the kind of fault behind CarriedIssues #1. | Check whether our drivers already have it (likely, by another route); if not, **PR E**. |
| ESP-IDF 5 / Arduino core 3 compatibility, WDT init under IDF 5 | pioarduino | A platform upgrade: newer NimBLE and Bluetooth stack, and a lot of change at once. | Backlog -- its own PR, not mixed with PR E's fixes. |
| Native Linux builds: selectable modem, KISS over TCP, Luckfox | native | The firmware on a Linux board with an SPI radio -- a possible route for IMPR-Vox (Pi Zero 2 W). | Backlog, for the Vox phase. |
| RAK4631/RAK13302 boards, SD and external flash storage | rak4631 / sx1262 | Not our hardware. | No. |

## The web console (`web_server`, already in our tree)

Two layers, both present, neither built for our boards:

- `Console.h` (Mark Qvist, 2024): the board starts its own Wi-Fi access point
  and serves an offline site from flash on port 80 -- the Reticulum manual and
  software downloads.
- `WebSocketServer` / `WebSocketConsole` (Chad Attermann, June 2026): a
  WebSocket on port 81 carrying the board's KISS control protocol -- what
  `rnodeconf` and Sideband use over USB -- plus Node Config pages. A browser can
  configure and drive the node.

Off here: `ENABLE_WEBSOCKETS` is commented out for the ESP32 builds (only the
native builds set it) and the RAD boards define `HAS_CONSOLE false`. It has **no
authentication**: anyone on the board's Wi-Fi controls the node, and our rule
is that fleet secrets are only ever typed on a terminal.

**As a mesh monitor it is the wrong shape**, for three reasons:

1. **Where the data is.** The deck already hears the whole mesh, and each board
   already publishes its state as NomadNet pages over Reticulum (`Pages.h`:
   interfaces, relaying, ESP-NOW, peers) -- readable over the mesh, from
   anywhere on it, with Reticulum identities rather than an open Wi-Fi AP.
2. **Memory.** A board serving HTTP and a Wi-Fi access point spends heap on a
   board we have just watched fragment to 1.3 KB in its largest block
   overnight.
3. **One board at a time.** A board's own page shows that board. Checking a
   mesh means seeing all of them together.

**So the proposal is a mesh status dashboard on the deck**, fed by what the
boards already publish -- their status pages and heap reports over Reticulum --
plus the deck's own path table: every node, its uptime and restarts, heap and
largest block, interfaces up, relaying, peers, and the route to it. It answers
"is the mesh healthy" in one glance before and during Outdoor Test 1, and it
is the data the PR F plugin's reachability view needs in ATAK.

**Scheduling:** the dashboard at the start of PR F (it serves Outdoor Test 1
and the plugin both). The board-local web console, if wanted as a field
provisioning and rescue tool for a spare board, after PR F -- with
authentication designed first and the fleet-secret rule kept.
