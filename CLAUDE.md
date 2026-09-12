# microReticulum_Firmware — QuakeMesh / IMPR-RAD

ESP32 LoRa / ESP-NOW / BLE mesh nodes running a microReticulum fork, for
disaster communications. Istanbul earthquake response, urban theatre.

Sibling repositories, developed together:

- `~/projects/columba` — Android Reticulum/LXMF app. Also ours. The Kotlin half
  of every codec in `tools/` lives in
  `app/src/main/java/network/columba/app/service/tak/`, and the two must stay
  byte-identical — `tak_native_v1.json` is copied between the repos and tested
  on both sides.
- `~/projects/urban-tak` — ATAK plugin for urban navigation, address lookup,
  rubble and building status. **Public repo**, no Reticulum dependency, and it
  must not acquire one. Runs in parallel with this work. Its
  `docs/MeshContract.md` fixes the seam: that repo decides what a thing means,
  this one decides how it travels.

## Read this before planning work

**`docs/TAKDeliveryPlan.md` is the single source of truth for what ships in
which PR.** It supersedes the sequence table at the end of `docs/TAKNative.md`.
Check it before proposing an order of work, and update it when the order
changes — the phasing must not live only in a conversation.

Design rationale and hardware acceptance records are in `docs/TAKNative.md` and
`docs/TAKIntegrationPivots.md`. Open hardware mysteries are in
`docs/CarriedIssues.md`.

## Standing constraints

- **Approved lab conditions: no duty-cycle limiting.** The zero airtime limit is
  intended, not an oversight. Gain (21) is the only legal constraint. Field
  approval for voice and video is sought separately.
- **Push to `origin` (DrLexus11) only.** `attermann` and `upstream` are
  push-disabled on purpose. Branch rather than committing to `master`.
- **Fleet secrets and IFAC passphrases are prompted on the terminal**, never
  accepted as command-line arguments — that keeps them out of shell history and
  process listings.
- **Scope discipline.** microReticulum, its library, Columba and the ATAK repo.
  Other projects' bugs are not ours to fix.
- **Disaster-first.** Weigh re-meshing speed against airtime; never inherit
  protocol politeness by default.

## Working notes

- Python tests run from the repo root: `python3 -m unittest discover -s tests -t
  tests`. Two tests read source files relative to the working directory and fail
  if run from inside `tests/`.
- Columba's Gradle default heap OOM-kills the build on this machine. Pass
  `-Dorg.gradle.jvmargs="-Xmx2560m -XX:MaxMetaspaceSize=768m"`, and use
  `:app:testNoSentryPythonBackendDebugUnitTest` — the bare `testDebugUnitTest`
  is ambiguous and `:screenshot-tests` fails to configure for unrelated reasons.
- `pgrep`/`pkill` patterns mismatch on this host and have killed the agent's own
  shell. Find the PID first, act on the number.
- Airtime figures come from `tools/position_budget.py`, which is the firmware's
  own `packet_airtime_ms()`. Use it rather than estimating.
