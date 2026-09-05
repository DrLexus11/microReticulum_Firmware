#!/usr/bin/env python3
"""Start the application on a board reached over the ESP32-S3 native USB.

An ESP32-S3 that presents its own USB-Serial/JTAG (303a:1001) exposes reset and
boot strapping on the CDC control lines. Opening the port can therefore leave
the board in the ROM downloader rather than the application, and it then
answers nothing: rnodeconf reports "did not respond", provisioning times out,
and the board looks bricked while being perfectly healthy.

The sequence below is the one that was measured to recover it, repeatedly, on
the second Rev 2 (N16R2). Deliberately stated as a measurement rather than a
polarity rule: the strapping behaviour depends on the open transition and on
whether the ROM or the application owns the endpoint, and a tidy story about
which line means what did not survive contact with the board. What does hold:

  - this sequence starts the application from the downloader;
  - tools/ifac/provision.py must open with DTR asserted, or the running
    application stops answering and ends up back in the downloader.

This walks it back out without touching the board:

    python tools/usb_jtag_boot.py /dev/ttyACM2

Confirm the downloader is gone by checking that esptool can no longer reach it:

    esptool.py --chip esp32s3 --port /dev/ttyACM2 --before no_reset read_mac

A successful connect means the ROM still owns the port; "No serial data
received" means the application is running.
"""

import argparse
import os
import time

import serial


def boot_application(port_path, settle=3.0):
    port = serial.Serial(baudrate=115200, timeout=0.3, dsrdtr=False, rtscts=False)
    port.port = port_path
    # IO0 must stay high for the whole pulse or the chip straps into the
    # downloader as EN is released.
    port.dtr = False
    port.rts = False
    port.open()
    port.dtr = False
    port.rts = True          # EN low
    time.sleep(0.2)
    port.rts = False         # EN high: boot
    port.close()
    # The reset re-enumerates the device; wait for the node to come back before
    # anything downstream tries to open it.
    deadline = time.time() + 25.0
    while time.time() < deadline and not os.path.exists(port_path):
        time.sleep(0.5)
    time.sleep(settle)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("port", help="board serial device, e.g. /dev/ttyACM2")
    parser.add_argument("--settle", type=float, default=3.0,
                        help="seconds to wait after re-enumeration")
    args = parser.parse_args()
    boot_application(args.port, settle=args.settle)
    print("reset issued on %s; the application should now own the port" % args.port)


if __name__ == "__main__":
    main()
