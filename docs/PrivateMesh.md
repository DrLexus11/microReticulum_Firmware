# Private Meshes, IFAC, and How Authorisation Layers

What "private" currently means on this firmware, what is missing, and how it
relates to the NomadNet pages and the resource API in `docs/ResourceAPI.md`.

Status: **IFAC is not implemented. This is the next substantial piece of work.**
Written 2026-08-23.

---

## 1. Three different questions, three different layers

Most confusion about "private mesh" comes from collapsing three separate
questions into one. They are answered at different layers and by different
mechanisms, and two of the three already work.

| Question | Layer | Mechanism | Status |
| --- | --- | --- | --- |
| Who may **read** my traffic? | end-to-end | Reticulum encryption | **works, always on** |
| Who may **call** my services? | application | identity + `ALLOW_LIST` | **works** |
| Who may **join and relay** on my network? | interface | **IFAC** | **not implemented** |

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

### Membership is open

Anyone with a radio on the same PHY can join, announce, be routed, **use your
nodes as transport**, and observe announce metadata -- which destinations exist,
hop counts, rough topology -- even though content stays opaque. There is
currently no admission control at all.

## 2. IFAC is the mechanism, and it is not implemented

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

**microReticulum does not implement it.** `Interface.h` declares
`Bytes _ifac_identity` and an accessor, but nothing ever assigns it, and the
enforcement path in `Transport.cpp` is a `// TODO` with the original Python
commented out:

```c
if (interface.ifac_identity()) {
// TODO
/*p
    ifac = interface.ifac_identity.sign(raw)[-interface.ifac_size:]
    ...
```

So the branch is dead code. Any claim that this firmware supports private
networks is false today.

## 3. The property that makes both use cases work

**IFAC applies per interface, not per network.** A single node can therefore
belong to a closed network on one interface while serving open clients on
another, because it holds the key and re-signs when relaying:

```
LoRa backbone        ->  IFAC (network_name + passphrase)   closed
SoftAP + TCP local   ->  no IFAC, or a per-building code    open to residents
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
- **Secure node (inter-agency).** IFAC on **every** interface, no open
  attachment. Nothing enters without the code.

Same firmware, same board, different configuration. Deciding which a given unit
is should be a provisioning-time choice, not an accident of which build it got.

## 5. Two risks worth naming before committing

**Bit-compatibility is the technical risk.** IFAC must match Python RNS exactly
-- signature, HKDF derivation, masking, and the header flag. Any divergence
means our nodes silently cannot talk to a Python `rnsd` peer on an IFAC
interface, and the symptom will look like a broken mesh rather than a crypto
mismatch. That is the same silent-failure signature that has cost this project
the most time. **Test against a real `rnsd` peer, not only node to node.**

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

## 7. What implementing IFAC involves

1. Port the sign/derive/mask path in `Transport.cpp` from the commented Python,
   and the inverse on receive.
2. Assign `_ifac_identity` from a configured network name and passphrase, per
   interface.
3. Expose `network_name` and `passphrase` per interface -- via provisioning, so
   a deployed node can be configured without a reflash.
4. Interop-test against Python `rnsd` before trusting node-to-node results.
5. Decide the key distribution and rotation story.

Steps 4 and 5 are where the project risk lives, not steps 1 to 3.
