#!/usr/bin/env python3
"""Non-resetting status check for the two RAD-01 boards in the Deck lab.

This deliberately does not open either serial port. Opening Rev 1's native USB
port resets and re-enumerates it, while Rev 2's bridge control lines can affect
EN/IO0 depending on how J3 is wired. The check uses sysfs-visible stable device
links, ICMP/TCP reachability, and the RNS daemon's recalled announce data.
"""

import argparse
import json
import os
import socket
import subprocess
import sys


BOARDS = {
    "rev1": {
        "serial": "/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_80:B5:4E:F4:C7:A4-if00",
        "ip": "192.168.1.54",
        "tcp_port": 4242,
        "pn_hash": "ba03aa75f8a136b1b6a74667c755727e",
        "env": "impr-rad01-rev1",
    },
    "rev2": {
        "serial": "/dev/serial/by-id/usb-Silicon_Labs_CP2102N_USB_to_UART_Bridge_Controller_9a255dc4f508f11185e86bc3813d7cb7-if00-port0",
        "ip": "192.168.1.88",
        "tcp_port": 4242,
        "pn_hash": "41fc2ab5e88d0b355d3c35fa60f4a22e",
        "env": "impr-rad01-rev2-uart",
    },
}

EXPECTED_TRANSFER_KB = 4
EXPECTED_SYNC_KB = 8
EXPECTED_COSTS = [16, 3, 18]


def ping(ip):
    result = subprocess.run(
        ["ping", "-c", "1", "-W", "1", ip],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )
    return result.returncode == 0


def tcp_open(ip, port):
    try:
        with socket.create_connection((ip, port), timeout=1.5):
            return True
    except OSError:
        return False


def serial_state(path):
    if not os.path.exists(path):
        return {"present": False, "path": path, "device": None, "busy": None}
    device = os.path.realpath(path)
    result = subprocess.run(
        ["fuser", device], capture_output=True, text=True, check=False
    )
    owners = (result.stdout + result.stderr).strip().split()
    return {
        "present": True,
        "path": path,
        "device": device,
        "busy": bool(owners),
        "owners": owners,
    }


def recalled_announces():
    try:
        import LXMF
        import RNS
        from RNS.vendor import umsgpack as msgpack
    except ImportError as error:
        return {}, "RNS/LXMF unavailable: %s" % error

    RNS.loglevel = RNS.LOG_ERROR
    RNS.Reticulum()
    out = {}
    for name, board in BOARDS.items():
        destination_hash = bytes.fromhex(board["pn_hash"])
        app_data = RNS.Identity.recall_app_data(destination_hash)
        parsed = None
        valid = False
        error = None
        if app_data is not None:
            try:
                parsed = msgpack.unpackb(app_data)
                valid = bool(LXMF.pn_announce_data_is_valid(app_data))
            except Exception as exc:  # malformed data belongs in the report
                error = str(exc)
        contract_ok = bool(
            valid
            and isinstance(parsed, list)
            and len(parsed) >= 6
            and parsed[3] == EXPECTED_TRANSFER_KB
            and parsed[4] == EXPECTED_SYNC_KB
            and parsed[5] == EXPECTED_COSTS
        )
        out[name] = {
            "present": app_data is not None,
            "valid": valid,
            "contract_ok": contract_ok,
            "data": parsed,
            "error": error,
        }
    return out, None


def main():
    parser = argparse.ArgumentParser(
        description="Check both Deck-lab RAD-01 boards without opening serial ports"
    )
    parser.add_argument("--json", action="store_true", help="emit machine-readable JSON")
    args = parser.parse_args()

    announces, announce_error = recalled_announces()
    report = {"boards": {}, "announce_error": announce_error}
    healthy = True
    for name, board in BOARDS.items():
        entry = {
            "environment": board["env"],
            "serial": serial_state(board["serial"]),
            "ip": board["ip"],
            "ping": ping(board["ip"]),
            "tcp_4242": tcp_open(board["ip"], board["tcp_port"]),
            "propagation_destination": board["pn_hash"],
            "announce": announces.get(name),
        }
        report["boards"][name] = entry
        healthy = healthy and bool(
            entry["serial"]["present"]
            and not entry["serial"]["busy"]
            and entry["ping"]
            and entry["tcp_4242"]
            and entry["announce"]
            and entry["announce"]["contract_ok"]
        )
    report["healthy"] = healthy

    if args.json:
        print(json.dumps(report, indent=2))
    else:
        for name, entry in report["boards"].items():
            serial = entry["serial"]
            announce = entry["announce"] or {}
            print(
                "%s: serial=%s%s ip=%s ping=%s tcp4242=%s pn=%s announce=%s"
                % (
                    name,
                    serial.get("device") or "MISSING",
                    " BUSY" if serial.get("busy") else "",
                    entry["ip"],
                    "ok" if entry["ping"] else "FAIL",
                    "ok" if entry["tcp_4242"] else "FAIL",
                    entry["propagation_destination"],
                    announce.get("data") if announce else "MISSING",
                )
            )
        if announce_error:
            print("announce check: %s" % announce_error)
        print("overall: %s" % ("HEALTHY" if healthy else "FAIL"))

    return 0 if healthy else 1


if __name__ == "__main__":
    sys.exit(main())
