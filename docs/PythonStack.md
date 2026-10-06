# The Python Reticulum stack: our forks, and how we keep them

Columba's Python backend (`rns-backend-py`) installs three packages by commit:
Reticulum (RNS), LXMF and ble-reticulum. Since 2026-10-04 they come from
DrLexus11's forks (Columba #18), and the forks are based on **markqvist's own
releases**, not on the intermediate fork. Decided with the operator on
2026-10-04 ("cut the middle man").

## The chain, and where we stand in it

```
markqvist/Reticulum, markqvist/LXMF        the originals: releases and tags
  -> torlando-tech/{Reticulum,LXMF}        Columba's author: patches for Android
       -> DrLexus11/{Reticulum,LXMF}       ours: markqvist release + our patch stack
torlando-tech/ble-reticulum -> DrLexus11/ble-reticulum   (an original, not a fork)
```

| Package | Pinned in Columba today | Our branch on the latest release | Latest upstream (2026-10-04) |
|---|---|---|---|
| RNS | 1.4.2 + 6 patches, `5b3a6ee` on `fix/embedded-runtime-hardening-1.4.2` | `drlexus11/rns-1.5.5` = markqvist `1.5.5` + the same 6 | 1.5.5 (306 commits after 1.4.2) |
| LXMF | 1.1.0 + 8 patches, `8912186` on `feature/external-stamp-hardening-1.1.0` | `drlexus11/lxmf-1.2.0` = markqvist `1.2.0` + the same 8 | 1.2.0 (19 commits after 1.1.0) |
| ble-reticulum | `07d9413` on `main` | -- | (torlando-tech's own project) |

LXMF 1.2.0 requires `rns>=1.5.5`: the two move together.

## The patch stack

Each patch we carry, why, and whether upstream has it. None of these is in
markqvist's releases as of 2026-10-04.

**RNS** (from torlando-tech; all applied cleanly onto 1.5.5):

1. close socket on connection failure to prevent a resource leak
2. catch RPC exceptions in `__update_phy_stats()`
3. context managers for ratchet file I/O and log file writes
4. harden embedded runtime teardown and telemetry
5. close lifecycle races found in review
6. roll back partial discovery worker startup

**LXMF** (from torlando-tech; all applied cleanly onto 1.2.0; their 12
Columba-integration tests pass against our RNS 1.5.5):

1. external stamp generator support for Android
2. `receiving_interface` passed to the delivery callback for opportunistic messages
3. hop count captured from the packet for opportunistic messages
4. tests covering Columba's integration hooks
5. hardened external stamp generation lifecycle
6. tests: canonical stamp boundary and replacement
7. discard cooperative cancellation before validation
8. bound external generator work counters

## The rules

1. **Base on markqvist's release tags**, never on the intermediate fork's
   branches. The intermediate fork is a source of patches, not our base.
2. **One branch per upstream release**: `drlexus11/<package>-<version>`. A new
   release is a new branch with the patch stack rebased onto it. **Never
   rewrite or delete a branch that Columba pins a commit on**; the old
   torlando-named branches stay for as long as anything pins them.
3. **Every carried patch is listed above**, with its origin and upstream
   status. The stack stays small; a patch upstream absorbs is dropped at the
   next rebase.
4. **No remote to an original.** Compare from a scratch mirror outside the
   repositories (below). Never push, branch or open a pull request there.
5. **Interop before every bump.** Nothing in our fleet has run RNS 1.5.x yet:
   the deck runs 1.4.2 / LXMF 1.1.1, Columba 1.4.2. A bump is tested against
   the boards (C++), the bridge and lxmd (Python, deck) and Columba (Python and
   Kotlin) before it ships.
6. **Specialise through versioned extensions** (TAKDeliveryPlan, "Direction
   after PR F"): base wire compatibility kept where it costs nothing, broken
   only where it buys something measured, and then in all three
   implementations, pinned by shared fixtures.

## The drift check

Monthly, and before each field test:

```
S=$(mktemp -d)
git clone -q --mirror https://github.com/markqvist/Reticulum.git $S/mq-Reticulum.git
git --git-dir=$S/mq-Reticulum.git describe --tags --abbrev=0 master        # latest release
git --git-dir=$S/mq-Reticulum.git log --oneline <our base tag>..master     # what we lack
git --git-dir=$S/mq-Reticulum.git cherry master <our branch>              # + = our patch not upstream
```

The same for LXMF. Security, transport and link fixes move the check to a
rebase; anything else waits for its scheduled PR.

## What upstream has that we want (1.4.2 -> 1.5.5, 1.1.0 -> 1.2.0)

- **LXMF**: medium-aware timeouts for slow links such as LoRa; a failed
  propagation-node path now triggers sync backoff (possibly our outbound
  peering that does not complete); a payload-size regression fixed.
- **RNS**: a Transport deadlock on `receipts_lock` and a remove-while-iterating
  fix; Link watchdog and Resource fixes; path requests batched and given more
  time when slow interfaces are online; faster IFAC; **interfaces that can be
  detached and attached live** -- what Columba's Python backend needs to switch
  an interface without Apply & Restart.

## Licence

RNS and LXMF are under the **Reticulum License**: MIT terms plus two
conditions -- the software "shall not be used in any kind of system which
includes amongst its functions the ability to purposefully do harm to human
beings", and not to create AI or machine-learning training datasets. The
copyright notice must be kept. ble-reticulum is MIT. Disaster response is
squarely within the licence; a defence client is not, for anything that
contains this code.
