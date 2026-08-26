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
   remaining attachment interfaces and close the non-RNS management surfaces;
   this branch deliberately implements the public-node posture first. The
   investigated follow-up design is recorded in §8.
3. Confirm whether the deployed Columba/Sideband interface editor exposes
   `network_name` and `passphrase` before relying on phone-side IFAC.
4. Follow up the direct-RNode LXMF identity/download instability independently.

## 8. Follow-up PR: TCP/UDP IFAC and the secure-node posture

Status: **implemented on `feature/tcp-udp-ifac-secure-node`; host tests,
Rev1/Rev2 builds and hardware acceptance pass.** Kept separate from the merged
LoRa IFAC PR so each security boundary remains reviewable.

### 8.1 Conclusion and boundary

TCP and UDP IFAC are reachable with roughly the same implementation effort as
LoRa IFAC. No further cryptographic work is required in microReticulum:
`Interface::enable_ifac()` already applies to every interface, supports the
Python wire format and accepts access codes from 1 to 64 bytes. The firmware
constructs its LoRa, UDP and TCP interface objects before provisioning is
loaded, then registers them with Transport afterwards, so TCP and UDP can use
the same boot-time, fail-closed application pattern as LoRa.

Adding IFAC to the two RNS interfaces is necessary but **not sufficient** to
call a node fully private. There are two separate IP-facing protocols:

| Surface | Port / transport | Purpose | IFAC coverage |
| --- | --- | --- | --- |
| `TCPServerInterface` | TCP 4242, RNS HDLC | Reticulum client attachment | Can use interface IFAC |
| `UDPInterface` | UDP 4242, raw RNS frames | Reticulum LAN segment | Can use interface IFAC |
| KISS control listener | TCP 7633, KISS | Host mode, legacy config and Provisioning | Bypasses RNS; IFAC cannot protect it |
| USB/UART | physical KISS | Bring-up and recovery | Bypasses RNS by design |
| Bluetooth serial | wireless KISS | Host/config attachment | Bypasses RNS; pairing is its only gate |

The secure-node PR therefore has two work streams: protect TCP/UDP Reticulum
traffic, and close wireless/network management paths that do not traverse
Reticulum. Physical USB/UART remains available as the recovery path.

### 8.2 Access-code sizes and the UDP receive defect

Use a fixed **16-byte IFAC for TCP and UDP**, matching Python RNS's defaults for
those interface types. Keep LoRa at 8 bytes: the firmware's `MTU` is 508 bytes,
which is exactly Reticulum's 500-byte packet MTU plus the existing radio access
code. The library's 8-byte and 16-byte implementations already match frames
generated by Python RNS 1.4.2.

TCP has sufficient storage today: `TCPServerInterface.h` declares a 1064-byte
hardware MTU and `TCPI_RX_BUFLEN` of 1064 bytes. One interface instance
multiplexes every connected TCP client as a shared segment. Consequently all
clients on that listener share one access-code configuration; per-client keys
would require separate interface instances and are outside the proposed scope.

UDP had a concrete truncation bug before this branch. Although
`UDPInterface` advertises a 1064-byte hardware MTU, `update_wifi()` in
`Remote.h` reads each datagram with the radio/KISS `MTU` limit of 508 bytes:

```
udp.read(udp_buffer.writable(MTU), MTU)
```

A maximum Reticulum packet protected by Python's 16-byte UDP IFAC is 516
bytes, so the old path truncated it before authentication. `UDP_RX_CAPACITY` is
now **564 bytes** (500-byte RNS MTU plus microReticulum's maximum 64-byte IFAC)
without changing the radio/KISS MTU. The receive path compares the announced
datagram length first and drains/rejects oversized or incomplete datagrams
instead of passing truncated data to Transport. The UDP send path already
writes the complete `RNS::Bytes` object and needed no equivalent change.

### 8.3 Provisioning design

Preserve Reticulum's per-interface semantics instead of turning the LoRa
namespace into a global key:

- retain namespace 109 as **LoRa Access Control**, with its 8-byte code;
- add namespace 110 as **TCP Access Control**;
- add namespace 111 as **UDP Access Control**; and
- give each new namespace the same reboot-required `Enabled`, `Network Name`
  and secret `Passphrase` fields as LoRa, deriving a 16-byte code.

The TCP and UDP credentials should be independent. An installation may choose
to reuse values, but the data model must not require it: a node is allowed to
bridge interfaces belonging to different private networks, re-authenticating
each outbound frame for that interface.

Factor the existing LoRa apply logic into a common helper. For every interface,
an enabled configuration with an empty name, an empty passphrase or failed key
derivation must leave that interface in `require_ifac(true)` state with no key,
so it fails closed. Reject incomplete commits as the current LoRa namespace
does. Keep all access-control fields reboot-required so a provisioning response
cannot be stranded by changing protection midway through the exchange.

Namespaces 110 and 111 now implement those independent TCP and UDP tuples, and
namespace 112 carries the single secure-node posture switch. The common apply
helper uses 8 bytes for LoRa and 16 bytes for TCP/UDP. Secure mode additionally
forces every RNS interface into protected or fail-closed operation even if an
individual Enabled flag is missing or corrupt.

`tools/ifac/provision.py` now addresses each interface independently. Its
`secure` operation discovers the IFAC namespaces compiled into the connected
board, then stages their credentials, the administrator allow-list,
remote-management enablement and secure posture in one SetState/Commit
transition. Provisioning persists namespaces as separate files, so this is not
a power-loss-atomic database transaction. The secure flag is deliberately
committed first: interruption can leave the node isolated, but not falsely
secure with wireless KISS open; physical serial remains the recovery path.
The microReticulum wire commit reports and stops on persistent-storage failure,
and the host treats that response as failure instead of verifying only the
updated RAM state. The failed namespace remains dirty so the commit can be
retried after storage recovers. Passphrases remain prompt-only and are checked
for omission from both draft and committed responses. The inverse `open`
transition disables secure mode last, after every advertised IFAC, preserving
fail-closed behaviour if recovery is interrupted. Individual IFAC disable
operations are rejected while secure mode is committed because secure mode
would otherwise continue requiring that IFAC and cleared credentials would
strand the interface.

### 8.4 Closing management paths

The ESP32 WiFi KISS listener is an unauthenticated `WiFiServer` on port 7633.
Bytes accepted there enter the shared serial FIFO and reach `serial_callback()`,
which handles legacy configuration, data and `CMD_PROVISION_REQ`. Protecting
TCP port 4242 or UDP port 4242 does nothing to this listener. Anyone who can
reach it has a configuration path unless it is disabled.

The secure-node posture now:

1. disable the WiFi KISS listener on port 7633;
2. disable Bluetooth KISS unless the deployment explicitly requires it and has
   an accepted pairing policy;
3. keep Reticulum remote management behind identity authentication and its
   allow-list;
4. retain physical USB/UART KISS for recovery; and
5. return to a documented recoverable/open state on factory reset.

These are one reboot-required transition, not unrelated toggles. WiFi and BLE
KISS start fail-closed while the LittleFS-backed policy is loading, avoiding a
brief unauthenticated boot window. Factory reset restores the open default.
The tool verifies complete interface credentials and the intended administrator
before reporting success, then names the physical serial device that remains as
the recovery transport.

The fallback SoftAP password is not a substitute for this work. It is derived
from the publicly observable MAC address using code present in the firmware,
so it keeps out accidental users but not a deliberate peer. A provisioned,
random per-node WPA2 key is useful defence in depth, but RNS IFAC and closure of
port 7633 are still required for the secure-node claim.

### 8.5 Compatibility constraint

Before enabling TCP IFAC on a resident-facing deployment, confirm that its
actual client can configure `network_name` and `passphrase`. This remains
unverified for Columba/Sideband. A client without an IFAC editor will be
correctly rejected by a protected TCP listener, so enabling the feature before
that check converts a security improvement into a client outage.

This constraint does not block the inter-agency secure-node implementation or
Python RNS acceptance. It does block making TCP IFAC the default for the public
QuakeMesh posture.

### 8.6 Acceptance plan for the follow-up PR

Run the existing microReticulum native suite first; all 19 IFAC tests and both
Python wire vectors must remain green. Then use the two RAD-01 boards and an
isolated Python RNS fixture, with USB on Rev 1 and UART plus the required hash
write on Rev 2 retained throughout recovery testing.

1. **UDP capacity:** pass open and protected frames at the 500-byte RNS packet MTU;
   prove a 516-byte protected datagram is received intact, and prove datagrams
   above the declared UDP capacity are dropped rather than truncated.
2. **TCP positive:** Python TCP with matching 16-byte IFAC discovers a path and
   completes an LXMF delivery through the firmware listener.
3. **TCP negative:** wrong-key and open clients receive no path/link success;
   tampered frames do not increment accepted receive counters or callbacks.
4. **UDP positive/negative:** repeat matching, wrong-key, open-peer and tamper
   cases with an isolated Python UDP interface.
5. **Bridge:** with LoRa, TCP and UDP protected, complete an end-to-end LXMF
   exchange across at least two interface types to prove re-signing at the
   boundary.
6. **Management closure:** confirm TCP 7633 is unreachable and Bluetooth KISS
   is unavailable in secure mode, while protected TCP 4242 and UDP 4242 still
   operate.
7. **Recovery:** change or disable protection over physical USB/UART, reboot and
   regain normal access. Verify factory reset also returns the documented
   recoverable state.
8. **Persistence:** power-cycle both revisions and repeat a protected positive
   exchange, then run `tools/rad01/lab_status.py` in the final intended state.

Record firmware hashes, exact interface configuration, Python RNS version and
positive/negative outcomes here, just as for the LoRa acceptance in §7. Do not
merge on path discovery alone: require a real bidirectional packet or LXMF
acknowledgement on each protected IP interface.

### 8.7 Hardware acceptance record (2026-08-26)

The follow-up implementation was exercised on both RAD-01 revisions with
Python RNS 1.4.2 and LXMF 1.1.1. Rev 1 used native USB at `/dev/ttyACM1`; Rev 2
used the CP2102 UART bridge at `/dev/ttyUSB0`, including the mandatory
post-upload power cycle and `fixhash` operation.

| Board | Environment | Firmware hash |
| --- | --- | --- |
| IMPR-RAD-01 Rev 1 | `impr-rad01-rev1` | `e5b2d4868d1c8857f5001cdad6649f1c3610c9241596cc44e06813372f680d62` |
| IMPR-RAD-01 Rev 2 | `impr-rad01-rev2-uart` | `24ebb4e37b86b081acc1727af2e39be36aae684e48ec174f08724a134ffce857` |

The temporary test tuples used distinct TCP and UDP names and passphrases.
Both were cleared after recovery. The pre-existing LoRa tuple remained enabled
under network name `IFAC interoperability test`; its write-only passphrase was
never read or replaced. The two existing remote-management administrators were
also left unchanged.

Results:

- **Startup listener regression:** the first hardware image exposed an
  address-ordering defect: TCP 7633 was started before DHCP and never acquired
  a working listener. `Remote.h` now tracks the bound address and recreates the
  listener when DHCP completes. Both boards then kept ports 4242 and 7633 open
  across repeated connect/disconnect probes in open mode.
- **Schemas and secrecy:** both physical transports advertised LoRa, TCP and
  UDP Access Control plus Secure Node. Passphrase remained absent from draft
  and committed responses.
- **TCP rejection:** isolated unprotected and wrong-key Python TCP fixtures
  connected to Rev 1's socket but obtained no path. A matching 128-bit IFAC
  completed a link and page request. A separate 766-byte propagated LXMF
  Resource was acknowledged by the node and downloaded byte-for-byte by its
  recipient.
- **UDP rejection and capacity:** isolated unprotected and wrong-key Python UDP
  fixtures obtained no path to Rev 2. A matching 128-bit IFAC completed a link
  and page request. A 1,166-byte propagated LXMF Resource was acknowledged and
  downloaded byte-for-byte, exercising multi-packet protected traffic and the
  516-byte maximum protected datagram path. The automated firmware contract
  also verifies the 564-byte receive capacity and the reject-before-dispatch
  branch for oversized datagrams; no live drop counter is currently exported.
- **Boundary re-signing:** with only the protected TCP fixture connected to Rev
  1, a link and page exchange with Rev 2 completed across Rev 1 TCP, the
  protected LoRa backbone and Rev 2. There was no direct host UDP route in that
  fixture.
- **Secure posture:** after secure-mode reboot, both boards refused TCP 7633
  while protected TCP 4242 and UDP continued to complete bidirectional page
  exchanges. Rev 2 was separately observed advertising as `RNode 87D8` after
  Bluetooth was enabled; after secure reboot it disappeared from an active
  BlueZ scan and 7633 was closed. Bluetooth was restored to its original
  disabled setting afterward.
- **Cold-boot persistence:** both boards were physically power-cycled in secure
  mode. The closed 7633 / reachable protected 4242 split persisted, and both
  protected page exchanges succeeded again.
- **Physical recovery:** USB on Rev 1 and UART on Rev 2 remained usable in
  secure mode. A staged recovery disabled secure mode and cleared only TCP/UDP,
  after which 7633 reopened and unprotected TCP and UDP page exchanges
  succeeded. `tools/rad01/lab_status.py` ended `HEALTHY` for both boards, and
  the normal host `rnsd` interfaces and LXMF daemon were restored.

Repository verification completed with 31 Python tests passing and successful
builds of both RAD environments. The microReticulum PlatformIO runner discovered
all 19 `test_ifac` cases but could not compile any on this host because its
native toolchain cannot resolve the system `stdio.h`, `setjmp.h` and
`features.h` headers. This is a host toolchain defect rather than a test
failure; the firmware builds compile the pinned microReticulum commit on both
ESP32-S3 targets. Repeat the 19 native cases in CI or on a host with a complete
native PlatformIO toolchain before merge.
