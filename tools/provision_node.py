#!/usr/bin/env python3
"""Bring a freshly flashed board onto the bench mesh, in one command.

Every board so far has needed the same four things after its first flash, and
each of them has been discovered the hard way at least once:

  1. The LoRa IFAC credentials. A board without them hears every frame on the
     air and joins nothing -- its own announces go out unmasked and the fleet
     drops them, while the fleet's masked frames arrive and fail to parse. The
     radio looks perfect the whole time. Enabling LoRa also enables ESP-NOW,
     which shares the credentials by design.

  2. The time authorities. A node refuses signed time from any key it was not
     told to trust, and reports it as "not an authority" rather than failing
     visibly. A board provisioned with only one of the three sits with a clock
     restored from storage while assertions arrive and are discarded.

  3. No stored Wi-Fi SSID, on a fixture that reaches the mesh over ESP-NOW.
     Remote.h resumes the channel sweep on failure only when no SSID is
     configured; with one it tries once at boot, fails, and never retries. That
     is the difference between a board that finds its way home and one that
     sits on channel 1 forever while the mesh is on channel 9.

  4. Its own identity read back, so it can be added to the deck's authority
     lists and found on the mesh.

The IFAC passphrase is read from a board backup and handed to the provisioner
inside this one process: never written to a file, never placed on a command
line, never printed.

    python tools/provision_node.py --port /dev/ttyUSB0

Add --keep-wifi for a board that should stay on the LAN (a RAD with
credentials), which skips step 3.
"""

import argparse
import hashlib
import os
import sys
import time

# Relative to this file, not to one checkout on one machine: the tool is meant
# to travel with the repo, and an absolute path silently breaks it for anyone
# who cloned somewhere else.
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "ifac"))

import serial  # noqa: E402  (after the path insert)
from RNS.vendor import umsgpack  # noqa: E402

import provision as P  # noqa: E402

# A machine-specific data file rather than part of the repo, so this stays an
# absolute default and --ifac-backup overrides it. It is only a default: the
# tool reports what it could not open rather than failing obscurely.
DEFAULT_BACKUP = "/home/deck/ozd_backup/fs_old/config/ns109.msgpack"

# Provisioning namespace 100, from Provisioning.h.
NS_GENERAL = 100
FIELD_TIME_PEER = 8
FIELD_TIME_AUTHORITIES = 9

# KISS, from Framing.h.
FEND = 0xC0
CMD_WIFI_SSID = 0x6B

# The bench's three time authorities. Rev 1 (NTP, stratum 1 and the LoRa/Wi-Fi
# relay), the deck daemon, and the phone running Columba.
DEFAULT_AUTHORITIES = [
    "60ba52911ee1ddbefc646a67dc969894",   # Rev 1
    "5b197a1c413f8ab930de8cfd0d8c7a71",   # deck, tools/time_authority.py
    "d7be13b33976f3458585dc3fd48c9783",   # Columba on the phone
]
# Rev 1's remote-management destination: what a node solicits UTC from.
DEFAULT_TIME_PEER = "a4f4dbd20b01de2d9b087a8d0afe1880"


def load_ifac(path):
    if not os.path.exists(path):
        raise SystemExit(
            "no IFAC backup at %s\n"
            "Pass --ifac-backup with a board backup holding the LoRa IFAC "
            "record." % path)
    with open(path, "rb") as handle:
        record = umsgpack.unpackb(handle.read())
    return record[P.FIELD_NETNAME], record[P.FIELD_PASSPHRASE]


def clear_wifi_ssid(port):
    """Clear the stored SSID over KISS. Reboot-required, like the rest."""
    handle = serial.Serial(baudrate=115200, timeout=0.3, dsrdtr=False, rtscts=False)
    handle.port = port
    handle.dtr = False
    handle.rts = False
    handle.open()
    time.sleep(0.5)
    # The handler commits on the first 0x00 and zero-fills the remaining bytes.
    handle.write(bytes([FEND, CMD_WIFI_SSID, 0x00, FEND]))
    handle.flush()
    time.sleep(1.0)
    handle.close()


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", required=True)
    parser.add_argument("--usb-jtag", action="store_true",
                        help="board is on the ESP32-S3 native USB-Serial/JTAG")
    parser.add_argument("--ifac-backup", default=DEFAULT_BACKUP,
                        help="board backup holding the LoRa IFAC record")
    parser.add_argument("--time-peer", default=DEFAULT_TIME_PEER)
    parser.add_argument("--authority", action="append", default=None,
                        help="repeatable; defaults to the bench's three")
    parser.add_argument("--keep-wifi", action="store_true",
                        help="leave the stored SSID alone")
    args = parser.parse_args()

    authorities = args.authority or DEFAULT_AUTHORITIES
    network, secret = load_ifac(args.ifac_backup)
    print("IFAC network : %r" % network)
    print("passphrase   : %d chars, sha256[:8]=%s (not shown)"
          % (len(secret), hashlib.sha256(secret.encode()).hexdigest()[:8]))

    client = P.KissProvisioner(args.port, usb_jtag=args.usb_jtag)
    try:
        state = client.request(P.OP_GET_STATE, {1: [P.NS_ADDRESSES]})
        values = state.get(1, {}).get(P.NS_ADDRESSES, {})
        labels = ("transport", "probe", "management", "nomadnet")
        identity = {label: bytes(values.get(index, b"")).hex()
                    for index, label in enumerate(labels, 1)}

        P.require_ifac_schema(client, "lora")
        P.provision(client, "lora", True, network, secret)
        print("LoRa/ESP-NOW IFAC committed")

        fields = {FIELD_TIME_PEER: bytes.fromhex(args.time_peer),
                  FIELD_TIME_AUTHORITIES: [bytes.fromhex(a) for a in authorities]}
        response = client.request(P.OP_SET_STATE, {3: {NS_GENERAL: fields}, 5: True})
        errors = response.get(3, []) if isinstance(response, dict) else []
        if errors:
            client.request(P.OP_DISCARD, [NS_GENERAL])
            raise SystemExit("SetState field errors: %r" % (errors,))
        client.request(P.OP_COMMIT, {1: [NS_GENERAL], 5: True})
        print("time peer and %d authorities committed" % len(authorities))
    finally:
        client.close()

    if not args.keep_wifi:
        clear_wifi_ssid(args.port)
        print("stored Wi-Fi SSID cleared -- ESP-NOW recovery will keep sweeping")

    print("\nreboot the board for these to take effect, then:")
    for label, value in identity.items():
        if value:
            print("  %-11s %s" % (label, value))
    print("\nAdd this node's identity to any authority list that should trust it.")


if __name__ == "__main__":
    main()
