#!/usr/bin/env python3
"""Provision interface IFAC and secure-node policy over physical KISS serial.

Use the repository's RNS virtual environment, which supplies pyserial and
umsgpack. Passphrases are prompted on the terminal and never accepted as command
line arguments, keeping them out of shell history and process listings.
"""

import argparse
import getpass
import sys
import time

import serial
from RNS.vendor import umsgpack


FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_PROVISION_REQ, CMD_PROVISION_RSP = 0x86, 0x87
CMD_RESET, CMD_RESET_BYTE = 0x55, 0xF8

OP_GET_SCHEMA, OP_GET_STATE = 1, 4
OP_SET_STATE, OP_COMMIT, OP_DISCARD = 5, 6, 7
NS_GENERAL = 1
FIELD_REMOTE_MANAGEMENT_ENABLED = 3
FIELD_REMOTE_MANAGEMENT_ALLOWED = 8
NS_IFAC_LORA, NS_IFAC_TCP, NS_IFAC_UDP = 109, 110, 111
NS_SECURE_NODE = 112
FIELD_ENABLED, FIELD_NETNAME, FIELD_PASSPHRASE = 1, 2, 3
FIELD_SECURE_NODE_ENABLED = 1
IFAC_NAMESPACES = {
    "lora": (NS_IFAC_LORA, "LoRa"),
    "tcp": (NS_IFAC_TCP, "TCP"),
    "udp": (NS_IFAC_UDP, "UDP"),
}
NS_RADIO = 101
FIELD_OP_MODE = 1
MODE_HOST, MODE_TNC = 0x11, 0x12
NS_ADDRESSES = 107
FF_SECRET = 1 << 3


def kiss_frame(command, payload=b""):
    framed = bytearray((FEND, command))
    for byte in payload:
        if byte == FEND:
            framed.extend((FESC, TFEND))
        elif byte == FESC:
            framed.extend((FESC, TFESC))
        else:
            framed.append(byte)
    framed.append(FEND)
    return bytes(framed)


class KissProvisioner:
    def __init__(self, port, boot_wait=4.0, timeout=6.0, usb_jtag=False):
        # On a board reached through the ESP32-S3's native USB-Serial/JTAG,
        # RTS drives EN and DTR drives IO0. pyserial asserts both by default,
        # which holds such a board in reset -- it answers nothing and looks
        # dead. Measured on the second Rev 2 (N16R2, 303a:1001): DTR asserted
        # keeps IO0 high for a normal boot, RTS deasserted releases EN, and any
        # other combination either resets it or drops it into the ROM
        # downloader. Boards behind a USB-serial bridge keep the old defaults.
        if usb_jtag:
            self.serial = serial.Serial(baudrate=115200, timeout=0.1,
                                        dsrdtr=False, rtscts=False)
            self.serial.port = port
            self.serial.dtr = True
            self.serial.rts = False
            self.serial.open()
        else:
            self.serial = serial.Serial(port, 115200, timeout=0.1)
        self.timeout = timeout
        self.sequence = 1
        ready_at = time.monotonic() + boot_wait
        while time.monotonic() < ready_at:
            self.serial.read(4096)

    def close(self):
        self.serial.close()

    def _read_frame(self, deadline):
        in_frame = False
        escaped = False
        frame = bytearray()
        while time.monotonic() < deadline:
            chunk = self.serial.read(1)
            if not chunk:
                continue
            byte = chunk[0]
            if byte == FEND:
                if in_frame and frame:
                    return bytes(frame)
                in_frame, escaped = True, False
                frame.clear()
            elif not in_frame:
                continue
            elif escaped:
                if byte == TFEND:
                    frame.append(FEND)
                elif byte == TFESC:
                    frame.append(FESC)
                else:
                    frame.append(byte)
                escaped = False
            elif byte == FESC:
                escaped = True
            else:
                frame.append(byte)
        raise TimeoutError("provisioning response timed out")

    def request(self, operation, payload=None):
        sequence = self.sequence
        self.sequence += 1
        envelope = [operation, sequence] if payload is None else [operation, sequence, payload]
        self.serial.write(kiss_frame(CMD_PROVISION_REQ, umsgpack.packb(envelope)))
        self.serial.flush()
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            frame = self._read_frame(deadline)
            if not frame or frame[0] != CMD_PROVISION_RSP:
                continue
            response = umsgpack.unpackb(frame[1:])
            if not isinstance(response, list) or len(response) < 3:
                continue
            if response[1] != sequence:
                continue
            if response[0] == 101:
                raise RuntimeError("device rejected request: %r" % (response[2],))
            if response[0] != operation:
                raise RuntimeError("unexpected response operation %r" % response[0])
            return response[2]
        raise TimeoutError("matching provisioning response timed out")

    def reboot(self):
        self.serial.write(kiss_frame(CMD_RESET, bytes((CMD_RESET_BYTE,))))
        self.serial.flush()


def require_ifac_schema(client, interface="lora"):
    namespace_id, label = IFAC_NAMESPACES[interface]
    schema = client.request(OP_GET_SCHEMA, {1: [namespace_id]})
    if not isinstance(schema, list) or len(schema) != 1:
        raise RuntimeError("firmware does not advertise %s Access Control" % label)
    namespace = schema[0]
    if len(namespace) < 4 or namespace[0] != namespace_id:
        raise RuntimeError("unexpected %s IFAC namespace schema" % label)
    fields = {entry.get(1): entry for entry in namespace[3] if isinstance(entry, dict)}
    if set(fields) != {FIELD_ENABLED, FIELD_NETNAME, FIELD_PASSPHRASE}:
        raise RuntimeError("unexpected IFAC field set: %r" % sorted(fields))
    if not (fields[FIELD_PASSPHRASE].get(4, 0) & FF_SECRET):
        raise RuntimeError("refusing to provision: passphrase is not marked SECRET")
    return namespace[1]


def available_ifac_schemas(client):
    """Return the IFACs compiled into this firmware after validating schemas."""
    requested = [namespace_id for namespace_id, _ in IFAC_NAMESPACES.values()]
    schema = client.request(OP_GET_SCHEMA, {1: requested})
    if not isinstance(schema, list):
        raise RuntimeError("unexpected IFAC schema response")
    advertised = {
        entry[0] for entry in schema
        if isinstance(entry, list) and entry and entry[0] in requested
    }
    available = tuple(
        interface for interface, (namespace_id, _) in IFAC_NAMESPACES.items()
        if namespace_id in advertised
    )
    for interface in available:
        require_ifac_schema(client, interface)
    if not available:
        raise RuntimeError("firmware does not advertise any RNS IFAC interfaces")
    return available


def stage_ifac(client, interface, enabled, network_name="", passphrase="", clear=False):
    namespace_id, _ = IFAC_NAMESPACES[interface]
    require_ifac_schema(client, interface)
    fields = {FIELD_ENABLED: enabled}
    if enabled or clear:
        fields[FIELD_NETNAME] = network_name
        fields[FIELD_PASSPHRASE] = passphrase

    response = client.request(OP_SET_STATE, {3: {namespace_id: fields}, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, [namespace_id])
        raise RuntimeError("SetState field errors: %r" % (errors,))

    # IncludeState is useful for atomic verification, but no response is ever
    # allowed to echo the secret draft back to a reader.
    drafts = response.get(5, {}) if isinstance(response, dict) else {}
    if FIELD_PASSPHRASE in drafts.get(namespace_id, {}):
        client.request(OP_DISCARD, [namespace_id])
        raise RuntimeError("firmware exposed a SECRET draft; changes discarded")
    return namespace_id


def provision(client, interface, enabled, network_name="", passphrase="", clear=False):
    if not enabled and secure_node_enabled(client):
        raise RuntimeError(
            "cannot disable an individual IFAC while secure-node posture is enabled; "
            "use the open operation"
        )
    namespace_id = stage_ifac(
        client, interface, enabled, network_name, passphrase, clear)

    committed = client.request(OP_COMMIT, {1: [namespace_id], 5: True})
    state = client.request(OP_GET_STATE, {1: [namespace_id]})
    values = state.get(1, {}).get(namespace_id, {})
    if bool(values.get(FIELD_ENABLED, False)) != enabled:
        raise RuntimeError("committed Enabled value did not verify")
    if enabled and values.get(FIELD_NETNAME) != network_name:
        raise RuntimeError("committed Network Name did not verify")
    if FIELD_PASSPHRASE in values:
        raise RuntimeError("firmware exposed a committed SECRET value")
    return committed


def require_secure_schema(client):
    schema = client.request(OP_GET_SCHEMA, {1: [NS_SECURE_NODE, NS_GENERAL]})
    by_id = {entry[0]: entry for entry in schema if isinstance(entry, list) and entry}
    secure = by_id.get(NS_SECURE_NODE)
    general = by_id.get(NS_GENERAL)
    if secure is None or general is None:
        raise RuntimeError("firmware does not advertise the secure-node schema")
    secure_fields = {entry.get(1): entry for entry in secure[3] if isinstance(entry, dict)}
    if set(secure_fields) != {FIELD_SECURE_NODE_ENABLED}:
        raise RuntimeError("unexpected Secure Node field set")
    general_fields = {entry.get(1): entry for entry in general[3] if isinstance(entry, dict)}
    for field in (FIELD_REMOTE_MANAGEMENT_ENABLED, FIELD_REMOTE_MANAGEMENT_ALLOWED):
        if field not in general_fields:
            raise RuntimeError("remote-management field %d is unavailable" % field)


def secure_node_enabled(client):
    """Read committed secure-node state; absent schemas are legacy/open."""
    schema = client.request(OP_GET_SCHEMA, {1: [NS_SECURE_NODE]})
    if not isinstance(schema, list) or not schema:
        return False
    state = client.request(OP_GET_STATE, {1: [NS_SECURE_NODE]})
    return bool(state.get(1, {}).get(NS_SECURE_NODE, {}).get(
        FIELD_SECURE_NODE_ENABLED, False))


def cmd_admin(client, args):
    """Add or replace the remote-management allow list, and nothing else.

    A node with an empty allow list refuses /status, /path and /time before the
    handler runs, so it answers nothing at all -- which is why a node with no
    clock could not simply be handed one. This grants that trust without
    touching secure-node posture or any IFAC, so it is safe to run on a node
    that is already deployed.
    """
    administrators = [parse_administrator(value) for value in args.identity]
    schema = client.request(OP_GET_SCHEMA, {1: [NS_GENERAL]})
    by_id = {entry[0]: entry for entry in schema
             if isinstance(entry, list) and entry}
    general = by_id.get(NS_GENERAL)
    if general is None:
        raise RuntimeError("firmware advertises no General Config namespace")
    fields = {entry.get(1): entry for entry in general[3]
              if isinstance(entry, dict)}
    if FIELD_REMOTE_MANAGEMENT_ALLOWED not in fields:
        raise RuntimeError(
            "this firmware exposes no remote-management allow list; it cannot "
            "be granted trust over the mesh")

    changes = {NS_GENERAL: {FIELD_REMOTE_MANAGEMENT_ALLOWED: administrators}}
    if FIELD_REMOTE_MANAGEMENT_ENABLED in fields:
        changes[NS_GENERAL][FIELD_REMOTE_MANAGEMENT_ENABLED] = True

    response = client.request(OP_SET_STATE, {3: changes, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, [NS_GENERAL])
        raise RuntimeError("SetState field errors: %r" % (errors,))
    client.request(OP_COMMIT, {1: [NS_GENERAL], 5: True})

    # Read back rather than trusting the commit: the allow list is the only
    # thing standing between this node and anyone claiming to know the time.
    state = client.request(OP_GET_STATE, {1: [NS_GENERAL]})
    stored = state.get(1, {}).get(NS_GENERAL, {}).get(
        FIELD_REMOTE_MANAGEMENT_ALLOWED, [])
    stored = [bytes(entry) for entry in stored]
    for administrator in administrators:
        if administrator not in stored:
            raise RuntimeError("allow list did not verify for %s"
                               % administrator.hex())
        print("allowed: %s" % administrator.hex())
    print("committed and verified -- reboot for it to take effect")


def parse_administrator(value):
    try:
        decoded = bytes.fromhex(value)
    except ValueError as error:
        raise RuntimeError("administrator must be hexadecimal") from error
    if len(decoded) != 16:
        raise RuntimeError("administrator must be a 16-byte identity hash")
    return decoded


def provision_secure(client, networks, passphrases, administrator, interfaces=None):
    """Stage all IFACs, the admin allow-list, and fail-closed secure posture."""
    require_secure_schema(client)
    interfaces = tuple(interfaces or available_ifac_schemas(client))
    for interface in interfaces:
        require_ifac_schema(client, interface)

    changes = {
        NS_GENERAL: {
            FIELD_REMOTE_MANAGEMENT_ENABLED: True,
            FIELD_REMOTE_MANAGEMENT_ALLOWED: [administrator],
        },
        NS_SECURE_NODE: {FIELD_SECURE_NODE_ENABLED: True},
    }
    for interface in interfaces:
        namespace_id, _ = IFAC_NAMESPACES[interface]
        changes[namespace_id] = {
            FIELD_ENABLED: True,
            FIELD_NETNAME: networks[interface],
            FIELD_PASSPHRASE: passphrases[interface],
        }

    # Persist the secure flag first. A power loss during the multi-file commit
    # can then isolate the RNS interfaces, but can never reboot into a posture
    # that was reported as secure while wireless KISS remains open. Physical
    # serial is deliberately retained for recovery.
    namespace_ids = [NS_SECURE_NODE, NS_GENERAL] + [
        IFAC_NAMESPACES[interface][0] for interface in interfaces
    ]
    response = client.request(OP_SET_STATE, {3: changes, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, namespace_ids)
        raise RuntimeError("secure-node SetState field errors: %r" % (errors,))
    drafts = response.get(5, {}) if isinstance(response, dict) else {}
    for interface in interfaces:
        namespace_id = IFAC_NAMESPACES[interface][0]
        if FIELD_PASSPHRASE in drafts.get(namespace_id, {}):
            client.request(OP_DISCARD, namespace_ids)
            raise RuntimeError("firmware exposed a SECRET draft; changes discarded")

    committed = client.request(OP_COMMIT, {1: namespace_ids, 5: True})
    state = client.request(OP_GET_STATE, {1: namespace_ids})
    values = state.get(1, {})
    if not values.get(NS_SECURE_NODE, {}).get(FIELD_SECURE_NODE_ENABLED, False):
        raise RuntimeError("secure-node Enabled value did not verify")
    admins = values.get(NS_GENERAL, {}).get(FIELD_REMOTE_MANAGEMENT_ALLOWED, [])
    if administrator not in admins:
        raise RuntimeError("administrator allow-list did not verify")
    for interface in interfaces:
        namespace_id, _ = IFAC_NAMESPACES[interface]
        iface = values.get(namespace_id, {})
        if not iface.get(FIELD_ENABLED, False):
            raise RuntimeError("%s IFAC Enabled value did not verify" % interface)
        if iface.get(FIELD_NETNAME) != networks[interface]:
            raise RuntimeError("%s IFAC Network Name did not verify" % interface)
        if FIELD_PASSPHRASE in iface:
            raise RuntimeError("firmware exposed a committed SECRET value")
    return committed


def provision_open(client, clear=False, interfaces=None):
    """Stage the fail-closed transition back to an open-node posture."""
    require_secure_schema(client)
    interfaces = tuple(interfaces or available_ifac_schemas(client))
    changes = {NS_SECURE_NODE: {FIELD_SECURE_NODE_ENABLED: False}}
    namespace_ids = [IFAC_NAMESPACES[interface][0] for interface in interfaces]
    namespace_ids.append(NS_SECURE_NODE)
    for interface in interfaces:
        namespace_id, _ = IFAC_NAMESPACES[interface]
        require_ifac_schema(client, interface)
        changes[namespace_id] = {FIELD_ENABLED: False}
        if clear:
            changes[namespace_id].update({FIELD_NETNAME: "", FIELD_PASSPHRASE: ""})
    response = client.request(OP_SET_STATE, {3: changes, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, namespace_ids)
        raise RuntimeError("open-node SetState field errors: %r" % (errors,))
    drafts = response.get(5, {}) if isinstance(response, dict) else {}
    for interface in interfaces:
        namespace_id = IFAC_NAMESPACES[interface][0]
        if FIELD_PASSPHRASE in drafts.get(namespace_id, {}):
            client.request(OP_DISCARD, namespace_ids)
            raise RuntimeError("firmware exposed a SECRET draft; changes discarded")

    committed = client.request(OP_COMMIT, {1: namespace_ids, 5: True})
    state = client.request(OP_GET_STATE, {1: namespace_ids})
    values = state.get(1, {})
    if values.get(NS_SECURE_NODE, {}).get(FIELD_SECURE_NODE_ENABLED, True):
        raise RuntimeError("secure-node Enabled value did not verify as disabled")
    for interface in interfaces:
        namespace_id, _ = IFAC_NAMESPACES[interface]
        iface = values.get(namespace_id, {})
        if iface.get(FIELD_ENABLED, True):
            raise RuntimeError("%s IFAC did not verify as disabled" % interface)
        if FIELD_PASSPHRASE in iface:
            raise RuntimeError("firmware exposed a committed SECRET value")
    return committed


def radio_state(client):
    schema = client.request(OP_GET_SCHEMA, {1: [NS_RADIO]})
    if not isinstance(schema, list) or len(schema) != 1:
        raise RuntimeError("firmware does not advertise RNode Radio Config")
    namespace = schema[0]
    fields = {entry.get(1): entry for entry in namespace[3] if isinstance(entry, dict)}
    mode = fields.get(FIELD_OP_MODE)
    if mode is None or set(mode.get(10, [])) != {MODE_HOST, MODE_TNC}:
        raise RuntimeError("firmware does not advertise host/tnc operating modes")
    state = client.request(OP_GET_STATE, {1: [NS_RADIO]})
    return state.get(1, {}).get(NS_RADIO, {})


def set_radio_mode(client, mode):
    radio_state(client)
    response = client.request(OP_SET_STATE, {3: {NS_RADIO: {FIELD_OP_MODE: mode}}, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, [NS_RADIO])
        raise RuntimeError("SetState field errors: %r" % (errors,))
    committed = client.request(OP_COMMIT, {1: [NS_RADIO], 5: True})
    if radio_state(client).get(FIELD_OP_MODE) != mode:
        raise RuntimeError("committed op_mode did not verify")
    return committed


def prompt_passphrase(label):
    secret = getpass.getpass("%s IFAC passphrase: " % label)
    if not secret:
        raise RuntimeError("passphrase must not be empty")
    confirm = getpass.getpass("Confirm %s passphrase: " % label)
    if secret != confirm:
        raise RuntimeError("passphrases do not match")
    return secret


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="board serial device")
    parser.add_argument("--boot-wait", type=float, default=4.0)
    parser.add_argument("--usb-jtag", action="store_true",
                        help="board is on the ESP32-S3 native USB-Serial/JTAG, "
                             "where RTS drives EN -- asserting it, as pyserial "
                             "does by default, holds the board in reset")
    sub = parser.add_subparsers(dest="action", required=True)

    enable = sub.add_parser("enable", help="stage and commit protected operation")
    enable.add_argument("--interface", choices=IFAC_NAMESPACES, default="lora")
    enable.add_argument("--network", required=True, help="IFAC network name")
    disable = sub.add_parser("disable", help="stage and commit an open interface")
    disable.add_argument("--interface", choices=IFAC_NAMESPACES, default="lora")
    disable.add_argument("--clear-credentials", action="store_true")
    schema = sub.add_parser("schema", help="verify access-control schema")
    schema.add_argument("--interface", choices=tuple(IFAC_NAMESPACES) + ("all",),
                        default="lora")
    secure = sub.add_parser("secure", help="enable the fail-closed fully private posture")
    secure.add_argument("--network", help="network name used for any unspecified interface")
    for interface in IFAC_NAMESPACES:
        secure.add_argument("--%s-network" % interface,
                            help="independent %s IFAC network name" % interface)
    secure.add_argument("--administrator", required=True,
                        help="16-byte remote-management identity hash in hex")
    secure.add_argument("--shared-passphrase", action="store_true",
                        help="prompt once and deliberately reuse the secret on all interfaces")
    opened = sub.add_parser("open", help="disable secure posture and all IFACs")
    opened.add_argument("--clear-credentials", action="store_true")
    sub.add_parser("radio", help="print current persisted radio settings")
    sub.add_parser("addresses", help="print the board's advertised destination hashes")
    mode = sub.add_parser("mode", help="stage and commit host or autonomous TNC mode")
    mode.add_argument("value", choices=("host", "tnc"))
    sub.add_parser("reboot", help="reboot after peers have all been committed")
    admin = sub.add_parser(
        "admin",
        help="set the remote-management allow list, without changing posture")
    admin.add_argument("identity", nargs="+",
                       help="16-byte identity hash(es) permitted to manage this "
                            "node -- including supplying it UTC via /time")
    args = parser.parse_args()

    client = KissProvisioner(args.port, boot_wait=args.boot_wait,
                             usb_jtag=args.usb_jtag)
    try:
        if args.action == "schema":
            selected = (available_ifac_schemas(client)
                        if args.interface == "all" else (args.interface,))
            names = [require_ifac_schema(client, interface) for interface in selected]
            if args.interface == "all":
                require_secure_schema(client)
            print("schema ok: %s" % ", ".join(names))
        elif args.action == "radio":
            values = radio_state(client)
            print("op_mode=%s frequency=%s bandwidth=%s sf=%s cr=%s txpower=%s implicit=%s" % (
                {MODE_HOST: "host", MODE_TNC: "tnc"}.get(values.get(1), values.get(1)),
                values.get(2), values.get(3), values.get(4), values.get(5), values.get(6),
                values.get(7)))
        elif args.action == "addresses":
            state = client.request(OP_GET_STATE, {1: [NS_ADDRESSES]})
            values = state.get(1, {}).get(NS_ADDRESSES, {})
            labels = ("transport", "probe", "management", "nomadnet")
            print(" ".join("%s=%s" % (label, bytes(values.get(index, b"")).hex())
                           for index, label in enumerate(labels, 1)))
        elif args.action == "mode":
            selected = MODE_HOST if args.value == "host" else MODE_TNC
            result = set_radio_mode(client, selected)
            print("op_mode=%s committed; reboot required: %s" % (
                args.value, bool(result.get(2, False))))
        elif args.action == "reboot":
            client.reboot()
            print("reboot requested")
        elif args.action == "admin":
            cmd_admin(client, args)
        elif args.action == "secure":
            interfaces = available_ifac_schemas(client)
            networks = {}
            for interface in interfaces:
                networks[interface] = getattr(args, "%s_network" % interface) or args.network
                if not networks[interface]:
                    raise RuntimeError("--%s-network or --network is required" % interface)
            administrator = parse_administrator(args.administrator)
            if args.shared_passphrase:
                shared = prompt_passphrase("Shared")
                passphrases = {interface: shared for interface in interfaces}
            else:
                passphrases = {
                    interface: prompt_passphrase(IFAC_NAMESPACES[interface][1])
                    for interface in interfaces
                }
            result = provision_secure(
                client, networks, passphrases, administrator, interfaces)
            print("secure-node posture committed; reboot required: %s" %
                  bool(result.get(2, False)))
            print("recovery after reboot: physical KISS serial on %s" % args.port)
            print("wireless KISS TCP 7633 and Bluetooth KISS will be disabled")
        elif args.action == "open":
            interfaces = available_ifac_schemas(client)
            result = provision_open(
                client, clear=args.clear_credentials, interfaces=interfaces)
            print("open-node posture committed; reboot required: %s" %
                  bool(result.get(2, False)))
            print("physical KISS serial on %s remains the recovery path until reboot" % args.port)
        elif args.action == "enable":
            label = IFAC_NAMESPACES[args.interface][1]
            secret = prompt_passphrase(label)
            result = provision(client, args.interface, True, args.network, secret)
            print("%s IFAC committed; reboot required: %s" %
                  (label, bool(result.get(2, False))))
        else:
            label = IFAC_NAMESPACES[args.interface][1]
            result = provision(client, args.interface, False,
                               clear=args.clear_credentials)
            print("%s IFAC disabled; reboot required: %s" %
                  (label, bool(result.get(2, False))))
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, TimeoutError) as error:
        sys.exit("error: %s" % error)
