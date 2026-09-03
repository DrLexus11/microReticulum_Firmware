# Bringing Eridanus's RRC client into Columba

Status: **proposed, deliberately not this branch.** Assessed 2026-09-03 against
`~/projects/eridanus` and `~/projects/columba`.

This branch's scope is the BLE peer interface and complete meshing over ESP-NOW.
RRC integration is a separate, larger piece of work and is recorded here so the
assessment is not repeated from scratch later.

## Why it is worth doing

Two apps exist for one network. Columba speaks LXMF, NomadNet, telephony and
telemetry; Eridanus speaks RRC and nothing else. A user who wants both carries
two clients against the same mesh and the same identity.

Our own firmware already serves RRC: `RRC_PROTOCOL_CORE` and `RRC_HUB` are
compiled into the RAD targets, and Rev 1 advertises a live hub at
`d36d1371772fca94fb6dc2522d1c4254`, with `tools/rrc/probe.py` to cross-check
against. So the integration can be tested against real hardware from day one.

## What makes it tractable

**The toolchains are identical.** AGP 9.1.0, Chaquopy 17.0.0, KSP 2.3.6,
`compileSdk 36`, JVM target 17, both MPL-2.0. The only difference found was the
Compose compiler plugin, 2.3.20 against Columba's 2.3.21.

**The two apps share an architecture**, almost certainly by common ancestry:

```
Columba:   :app :data :domain :micron :rns-api :rns-ipc :rns-host
           :rns-backend-kt :rns-backend-py :rns-stats
Eridanus:  :app       :eridanus-rns-api  :eridanus-rns-backend-kt
           :eridanus-rns-backend-py
```

**Columba contains no RRC code at all**, so this is purely additive.

**The screens already implement the intended flow**, close to one for one:

| Step | Eridanus screen |
| --- | --- |
| tab entry point | `ui/navigation/AppNavigation.kt` (195 lines) |
| list hubs | `HubBrowserScreen.kt` |
| connect a hub | `HostScreen.kt` |
| known rooms; create or join | `RoomListScreen.kt` |
| chat, with the full capability set | `ChatScreen` + `ChatLinks` + `MentionAutocomplete` + `SelfMention` |

`OnboardingScreen` and `SettingsScreen` have Columba equivalents already and
should not come across.

## The seam, and the one real gap

Eridanus's RRC layer is written against low-level Reticulum primitives. Columba
refactored its `RnsBackend` into domain facades (`core`, `lxmf`, `telephony`,
`telemetry`, `nomadnet`, `transportAdmin`). Measured dependency of the
2,527-line `rrc` package on the older API:

```
RnsLink 47   RnsIdentity 11   RnsResource(+Strategy/Advertisement) 10
RnsDestination(+Type/Direction) 10   RnsBackend 4
```

`RnsCore` already provides identities, destinations, links, packets and paths --
in a `suspend`/`Result`/`Flow` idiom rather than Eridanus's factory objects. So
one adapter implementing `RnsLink`, `RnsIdentity`, `RnsDestination` over
`RnsCore` lets the RRC package compile essentially unchanged.

**The gap is Resources.** Columba exposes no generic resource API. The
capability exists -- NomadNet page fetches are Resource transfers, with progress
flows, handled in `rns-backend-py` -- but only behind `RnsNomadnet`. Surfacing a
generic resource primitive is plumbing an existing Python path outward, not new
protocol work, but it is the one piece that is not a straight adaptation.

Volume: roughly 2,500 lines of RRC, 3,000 of screens, 1,800 of view model and
200 of navigation, plus an adapter estimated at 400-700.

## Phasing

**Phase 1 -- plumbing, no UI.** The adapter plus the `rrc` package, connecting
to Rev 1's live hub and listing rooms with no interface at all. A hub connect
and room list from inside Columba is a hard, verifiable milestone and it proves
the adapter before any UI work is spent on it.

**Phase 2 -- the tab.** `HubBrowser -> Host -> RoomList -> Chat` behind a new
bottom-bar entry, retheming to Columba's design system as each screen lands.

**Phase 3 -- the chat surface**, where Eridanus's mention and link handling
earns its place, and where UX changes belong.

## Constraints to respect

- **One Reticulum stack.** Do not bring `eridanus-rns-backend-kt` or
  `-py` across. Two backends in one app means two instances and two identities.
  Everything must sit on Columba's existing backend.
- **Keep the MPL-2.0 SPDX headers** on every moved file.
- Package rename `tech.torlando.eridanus` -> Columba's namespace.
