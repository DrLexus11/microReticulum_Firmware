#!/usr/bin/env python3
"""Put a board on a Wi-Fi network, the password asked for on the terminal.

The SSID and password go to the board's configuration over KISS
(CMD_WIFI_SSID, CMD_WIFI_PSK, then CMD_WIFI_MODE station), which is what
rnodeconf --ssid/--psk does -- except that rnodeconf takes the password on its
command line, into the shell's history and the process list. Here it is typed
at a prompt that does not echo, held in this process only, and never printed.

    python tools/set_wifi.py --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<MAC>-if00

Stop any soak logger on the port first. The board joins the network at once;
this waits for it to say so.
"""

import argparse
import getpass
import re
import sys
import time

import serial

FEND, FESC, TFEND, TFESC = 0xC0, 0xDB, 0xDC, 0xDD
CMD_WIFI_MODE, CMD_WIFI_SSID, CMD_WIFI_PSK = 0x6A, 0x6B, 0x6C
WR_WIFI_STA = 0x01
FIELD_MAX = 32          # the board keeps 32 bytes of each


def frame(command, data):
    escaped = bytearray()
    for byte in data:
        if byte == FEND:
            escaped += bytes([FESC, TFEND])
        elif byte == FESC:
            escaped += bytes([FESC, TFESC])
        else:
            escaped.append(byte)
    return bytes([FEND, command]) + bytes(escaped) + bytes([FEND])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    ssid = input("Wi-Fi network (SSID): ").encode()
    password = getpass.getpass("Wi-Fi password (not shown): ").encode()
    for name, value in (("SSID", ssid), ("password", password)):
        if not value or len(value) > FIELD_MAX:
            print("%s must be 1-%d bytes" % (name, FIELD_MAX), file=sys.stderr)
            return 1

    link = serial.Serial(baudrate=args.baud, timeout=0.5, dsrdtr=False, rtscts=False)
    link.port = args.port
    link.dtr = True          # held, so opening the port does not reset the board
    link.rts = True
    link.open()
    try:
        link.write(frame(CMD_WIFI_SSID, ssid + b"\x00"))
        link.flush()
        time.sleep(0.3)
        link.write(frame(CMD_WIFI_PSK, password + b"\x00"))
        link.flush()
        time.sleep(0.3)
        link.write(frame(CMD_WIFI_MODE, bytes([WR_WIFI_STA])))
        link.flush()
        password = None
        print("sent; waiting up to 40 s for the board to join %s ..." % ssid.decode(errors="replace"))
        deadline, seen = time.time() + 40, b""
        while time.time() < deadline:
            try:
                seen += link.read(4096)
            except serial.SerialException:
                # Something else is reading the port -- a soak logger. The
                # settings were written before this; only the confirmation is lost.
                print("settings sent, but another program is reading this port (a soak "
                      "logger?), so the board's confirmation cannot be read here", file=sys.stderr)
                return 2
            text = re.sub(rb"[^\x20-\x7e\n]", b"", seen).decode()
            if re.search(r"\[WiFi\] status: 3\b", text):
                print("joined (WiFi status 3)")
                return 0
        print("no confirmation seen; check the board's [WiFi] status lines", file=sys.stderr)
        return 1
    finally:
        link.close()


if __name__ == "__main__":
    sys.exit(main())
