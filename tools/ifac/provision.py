#!/usr/bin/env python3
"""Provision LoRa IFAC directly over a board's KISS serial connection.

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
NS_IFAC_LORA = 109
FIELD_ENABLED, FIELD_NETNAME, FIELD_PASSPHRASE = 1, 2, 3
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
    def __init__(self, port, boot_wait=4.0, timeout=6.0):
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


def require_ifac_schema(client):
    schema = client.request(OP_GET_SCHEMA, {1: [NS_IFAC_LORA]})
    if not isinstance(schema, list) or len(schema) != 1:
        raise RuntimeError("firmware does not advertise LoRa Access Control")
    namespace = schema[0]
    if len(namespace) < 4 or namespace[0] != NS_IFAC_LORA:
        raise RuntimeError("unexpected IFAC namespace schema")
    fields = {entry.get(1): entry for entry in namespace[3] if isinstance(entry, dict)}
    if set(fields) != {FIELD_ENABLED, FIELD_NETNAME, FIELD_PASSPHRASE}:
        raise RuntimeError("unexpected IFAC field set: %r" % sorted(fields))
    if not (fields[FIELD_PASSPHRASE].get(4, 0) & FF_SECRET):
        raise RuntimeError("refusing to provision: passphrase is not marked SECRET")
    return namespace[1]


def provision(client, enabled, network_name="", passphrase="", clear=False):
    require_ifac_schema(client)
    fields = {FIELD_ENABLED: enabled}
    if enabled or clear:
        fields[FIELD_NETNAME] = network_name
        fields[FIELD_PASSPHRASE] = passphrase

    response = client.request(OP_SET_STATE, {3: {NS_IFAC_LORA: fields}, 5: True})
    errors = response.get(3, []) if isinstance(response, dict) else []
    if errors:
        client.request(OP_DISCARD, [NS_IFAC_LORA])
        raise RuntimeError("SetState field errors: %r" % (errors,))

    # IncludeState is useful for atomic verification, but no response is ever
    # allowed to echo the secret draft back to a reader.
    drafts = response.get(5, {}) if isinstance(response, dict) else {}
    if FIELD_PASSPHRASE in drafts.get(NS_IFAC_LORA, {}):
        client.request(OP_DISCARD, [NS_IFAC_LORA])
        raise RuntimeError("firmware exposed a SECRET draft; changes discarded")

    committed = client.request(OP_COMMIT, {1: [NS_IFAC_LORA], 5: True})
    state = client.request(OP_GET_STATE, {1: [NS_IFAC_LORA]})
    values = state.get(1, {}).get(NS_IFAC_LORA, {})
    if bool(values.get(FIELD_ENABLED, False)) != enabled:
        raise RuntimeError("committed Enabled value did not verify")
    if enabled and values.get(FIELD_NETNAME) != network_name:
        raise RuntimeError("committed Network Name did not verify")
    if FIELD_PASSPHRASE in values:
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="board serial device")
    parser.add_argument("--boot-wait", type=float, default=4.0)
    sub = parser.add_subparsers(dest="action", required=True)

    enable = sub.add_parser("enable", help="stage and commit protected LoRa operation")
    enable.add_argument("--network", required=True, help="IFAC network name")
    disable = sub.add_parser("disable", help="stage and commit open LoRa operation")
    disable.add_argument("--clear-credentials", action="store_true")
    sub.add_parser("schema", help="verify that the IFAC schema is present")
    sub.add_parser("radio", help="print current persisted radio settings")
    sub.add_parser("addresses", help="print the board's advertised destination hashes")
    mode = sub.add_parser("mode", help="stage and commit host or autonomous TNC mode")
    mode.add_argument("value", choices=("host", "tnc"))
    sub.add_parser("reboot", help="reboot after peers have all been committed")
    args = parser.parse_args()

    client = KissProvisioner(args.port, boot_wait=args.boot_wait)
    try:
        if args.action == "schema":
            print("schema ok: %s" % require_ifac_schema(client))
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
        elif args.action == "enable":
            secret = getpass.getpass("LoRa IFAC passphrase: ")
            if not secret:
                raise RuntimeError("passphrase must not be empty")
            confirm = getpass.getpass("Confirm passphrase: ")
            if secret != confirm:
                raise RuntimeError("passphrases do not match")
            result = provision(client, True, args.network, secret)
            print("IFAC committed; reboot required: %s" % bool(result.get(2, False)))
        else:
            result = provision(client, False, clear=args.clear_credentials)
            print("IFAC disabled; reboot required: %s" % bool(result.get(2, False)))
    finally:
        client.close()
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, TimeoutError) as error:
        sys.exit("error: %s" % error)
