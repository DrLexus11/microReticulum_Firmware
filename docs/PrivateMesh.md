# Private Meshes, IFAC, and How Authorisation Layers

What "private" currently means on this firmware, what is implemented, and how it
relates to the NomadNet pages and the resource API in `docs/ResourceAPI.md`.

Status: **implemented and hardware-verified on `feature/IFAC-interop`.**
Originally written 2026-08-23; acceptance completed 2026-08-25.

**Priority history:** on 2026-08-24 the ESP32 LXMF propagation node ranked ahead
of this work. That acceptance run is now complete (see `docs/Messaging.md`), so
IFAC became the next branch on 2026-08-25.

---

## 1. Three different questions, three different layers

Most confusion about "private mesh" comes from collapsing three separate
questions into one. They are answered at different layers and by different
mechanisms, and two of the three already work.

| Question | Layer | Mechanism | Status |
| --- | --- | --- | --- |
| Who may **read** my traffic? | end-to-end | Reticulum encryption | **works, always on** |
| Who may **call** my services? | application | identity + `ALLOW_LIST` | **works** |
| Who may **join and relay** on my network? | interface | **IFAC** | **implemented and hardware-verified** |

Getting this straight matters commercially as much as technically: "the network
is open" and "the traffic is readable" are very different statements, and the
first is often mistaken for the second in a procurement conversation.

### Content is already private

Every packet is encrypted end to end between destinations. A transport node --
ours, or a stranger's -- cannot read what it relays. This holds regardless of
who is on the network, which is why an open mesh is not the same as an insecure
one.

### Services are already gated

Request handlers take `RNS::Type::Destination::ALLOW_LIST` plus a list of
identity hashes, checked against the identity a link proves via `identify()`.
This is what restricts the management pages today
(`RNS::Transport::remote_management_allowed()`). It is real authorisation
against a cryptographic identity, not a shared secret.

### Membership is configurable

With LoRa IFAC disabled (the factory default), anyone with a radio on the same
PHY can join, announce, be routed, **use your nodes as transport**, and observe
announce metadata. Enabling the provisioned LoRa access code drops frames that
are open, use the wrong key, or have been modified.

## 2. IFAC implementation

Reticulum's answer to closed membership is **IFAC** (Interface Access Codes).
Each interface carries a network name and passphrase; frames are signed and
masked with a derived key, and traffic without a valid code is dropped at the
interface.

In Python RNS it is configured per interface:

```
[[Some Interface]]
  type = TCPClientInterface
  network_name = <name>        # or: networkname
  passphrase   = <secret>      # or: pass_phrase
```

The `feature/IFAC-interop` branches implement the complete per-interface path:

- Python-compatible SHA-256/HKDF key derivation from `network_name` and
  `passphrase`;
- Ed25519 access-code signing, header flagging and masking on transmit;
- unmasking and constant-time access-code comparison before receive counters,
  callbacks or packet parsing;
- rejection of protected frames on open interfaces and open frames on protected
  interfaces; and
- a fail-closed state for a radio whose persisted configuration says IFAC is
  required but whose key cannot be derived.

The firmware provisions an 8-byte IFAC on the **LoRa interface only**. TCP and
UDP remain open local attachment paths. The library also supports other valid
sizes and is covered by both 8-byte and 16-byte Python-generated wire vectors.

## 3. The property that makes both use cases work

**IFAC applies per interface, not per network.** A single node can therefore
belong to a closed network on one interface while serving open clients on
another, because it holds the key and re-signs when relaying:

```
LoRa backbone        ->  IFAC (network_name + passphrase)   closed
SoftAP + TCP local   ->  no IFAC                            open to residents
```

A resident's phone never needs the backbone key. An unauthorised radio on the
same frequency cannot join the backbone or relay through it. **IFAC does not
block messaging**: LXMF rides end to end on top of routing, so as long as every
interface along a path shares a code or has none, messaging is unaffected.

## 4. Two deployment postures, one hardware

The bridging property above has a direct consequence that belongs in the product
definition rather than being discovered later:

- **Public node (QuakeMesh).** IFAC on the LoRa backbone, open local
  attachment. Residents attach with no secret. Note this deliberately lets
  anyone who reaches the SoftAP inject traffic into the backbone -- that *is*
  the product: a resident's message reaching responders.
- **Secure node (inter-agency, future).** IFAC on **every** interface, no open
  attachment. Nothing enters without the code.

The current branch implements the public-node posture: protected LoRa plus open
local attachment. Making the secure posture another provisioning choice remains
outstanding; it must not be implied merely because the radio is protected.

## 5. Two risks worth naming before committing

**Bit-compatibility is the technical risk.** IFAC must match Python RNS exactly
-- signature, HKDF derivation, masking, and the header flag. Any divergence
means our nodes silently cannot talk to a Python `rnsd` peer on an IFAC
interface, and the symptom will look like a broken mesh rather than a crypto
mismatch. Deterministic frames generated by installed Python RNS 1.4.2 match
byte-for-byte in both directions. Python RNS and Rev 1↔Rev 2 radio acceptance
are recorded in §7.

**Key distribution is the operational risk, and it is the larger one.** The
algorithm is bounded work; getting a passphrase onto every authorised node,
rotating it, and revoking a lost unit is what decides whether this survives
contact with an actual agency. Worth designing alongside the code, not after.

Unverified: whether Sideband/Columba exposes `network_name` and `passphrase` in
its interface editor. It likely does, but confirm on the device before designing
a deployment that assumes it.

## 6. Relationship to NomadNet and the resource API

`docs/ResourceAPI.md` proposes a schema-driven, validated resource API. It sits
at the **application** layer, alongside the NomadNet pages, and neither replaces
nor competes with IFAC:

- **NomadNet pages** are the human-facing surface: micron markup, browsed from
  Sideband/Columba.
- **The resource API** is the machine-facing surface: MsgPack, typed schema,
  validation, and transactional commits.
- **Both** authorise the same way -- by proven identity against `ALLOW_LIST`.
  The API must reuse that mechanism rather than inventing a second one, or a
  device ends up with two answers to "who may change this".
- **IFAC sits underneath both**, deciding who may exchange packets at all.

The practical ordering: a device on a public mesh needs the application-layer
gate to be right, because anyone can reach it. A device on an IFAC network still
needs it, because "on the network" is not the same as "authorised to reconfigure
the radio". IFAC reduces exposure; it does not remove the need for
authorisation.

## 6a. The fallback AP key is not a secret

Worth stating plainly, because it looks like one. The SoftAP PSK is derived from
the MAC by an algorithm that ships in open-source firmware, and the MAC is
broadcast in every WiFi frame. **Anyone who can see the access point can compute
its key.**

It is still better than a fleet-wide shared key -- one resident's password no
longer opens every neighbour's node, and it removes the "everyone knows the
building password" failure -- but it is a speed bump, not a credential. As of
2026-08-23 it is also no longer printed in logs by default
(`-DWIFI_AP_LOG_PSK` to print it for labelling), since log output reaches the
unauthenticated KISS console on port 7633. That closes an incidental leak; it
does not change the paragraph above.

A genuinely secret AP key has to be **provisioned per node** rather than
derived, which lands the same key-distribution problem described in §5. If the
fallback AP is ever expected to carry sensitive traffic, that work is a
prerequisite -- and note it should not have to, since Reticulum above it is
already end-to-end encrypted.

## 7. Configuration and acceptance

The Console exposes a root **LoRa Access Control** namespace with three
reboot-required fields: **Enabled**, **Network Name**, and **Passphrase**. Set
both strings, enable IFAC, commit, and reboot only after every radio peer has
the same values staged. An enabled commit without both strings is rejected.
The passphrase is marked `SECRET`, so it is omitted from GetState responses,
but it is currently stored as MsgPack on LittleFS without encryption at rest.
Physical possession of a node therefore still exposes the shared credential.

Acceptance on 2026-08-25 used the two physical IMPR-RAD-01 boards and installed
Python RNS 1.4.2:

- Rev 1 and the Rev 2 UART build compiled, flashed and passed their required
  post-upload hash writes. The running firmware hashes were
  `0138bd05f08651b8240e283d1aa7514b4094d5d6dc513d9b1f63c71903b4d293`
  and `37578c931d113f72164a387815999c52d358d5578c334ef77878a4139af91919`.
- With the Deck `rnsd`/UDP shortcuts stopped, a Python TCP client attached only
  to Rev 1 completed two exact LXMF store-and-fetch round trips against Rev 2
  over the two-hop LoRa path with matching IFAC.
- A wrong Rev 2 passphrase and an open Rev 2 against protected Rev 1 each timed
  out at link establishment with no acknowledgement. The host unit suite also
  rejects a modified frame before receive counters or callbacks.
- Disabling IFAC on both boards restored an open two-hop store-and-fetch. Both
  boards were then returned to matching protected operation and TNC mode; the
  normal lab health check passed.
- Rev 1 was temporarily placed in host mode and opened by Python as a native
  `RNodeInterface` with Python-side IFAC. Python obtained a one-hop path from
  Rev 2 and received Rev 2's delivery acknowledgement for a real LXMF upload,
  proving protected packets in both directions between Python and firmware.
- The complete microReticulum native suite passed 19/19 tests, including the
  8-byte and 16-byte Python wire vectors, mismatch/tamper drops and secret-state
  omission.

One higher-layer observation is deliberately not hidden: an immediate LXMF
download on the direct Python RNode fixture returned `PR_NO_IDENTITY_RCVD`, and
a later NomadNet link attempt timed out. Both happen after IFAC acceptance; the
path response and delivery acknowledgement above already prove bidirectional
IFAC interoperability. Track the link/identity behaviour separately instead of
conflating it with access-code compatibility.

Remaining product work:

1. Define fleet key distribution, rotation, and lost-unit revocation. The
   firmware provides configuration mechanics, not an operational key ceremony.
2. If the inter-agency posture is required, add provisioning for IFAC on the
   remaining attachment interfaces; this branch deliberately implements the
   public-node posture first.
3. Confirm whether the deployed Columba/Sideband interface editor exposes
   `network_name` and `passphrase` before relying on phone-side IFAC.
4. Follow up the direct-RNode LXMF identity/download instability independently.
