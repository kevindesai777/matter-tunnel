#!/usr/bin/env python3
"""
Capture a device console for a fixed duration and print it.

Deliberately not `stty` + `cat`: on macOS `stty -f <port>` closes the port, and
termios resets the line to 9600 before `cat` reopens it, so the capture is
garbage at the device's actual 115200. pyserial sets the baud rate on the same
handle it reads from, which is the only reliable way.

  serial_cap.py /dev/ttyACM1 35 > boot.log

SPDX-License-Identifier: Apache-2.0
"""
import sys
import time

import serial  # Debian/RPi OS: apt install python3-serial (Trixie is PEP-668)

port = sys.argv[1]
seconds = float(sys.argv[2])

s = serial.Serial(port, 115200, timeout=1)
end = time.time() + seconds
buf = b""
while time.time() < end:
    buf += s.read(4096)
s.close()
print(buf.decode(errors="replace"))
