#!/usr/bin/env python3
"""Minimal Spinel-over-HDLC probe: verify an RCP responds and report its versions.

Sends CMD_PROP_VALUE_GET for PROP_PROTOCOL_VERSION, PROP_NCP_VERSION and
PROP_CAPS, then decodes the PROP_VALUE_IS responses. No OpenThread host stack
required -- this runs anywhere pyserial does.

SPDX-License-Identifier: Apache-2.0
"""
import sys, time, serial

FLAG, ESC, ESC_XOR = 0x7E, 0x7D, 0x20
SPECIAL = {0x7E, 0x7D, 0x11, 0x13}

CMD_PROP_VALUE_GET, CMD_PROP_VALUE_IS = 2, 6
PROP = {1: "PROTOCOL_VERSION", 2: "NCP_VERSION", 3: "INTERFACE_TYPE", 5: "CAPS", 0: "LAST_STATUS"}


def fcs(data):                      # CRC-16/CCITT (X.25 FCS-16)
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return crc ^ 0xFFFF


def encode(payload):
    out = bytearray([FLAG])
    body = bytes(payload) + fcs(payload).to_bytes(2, "little")
    for b in body:
        if b in SPECIAL:
            out += bytes([ESC, b ^ ESC_XOR])
        else:
            out.append(b)
    out.append(FLAG)
    return bytes(out)


def decode(buf):
    frames, cur, esc = [], bytearray(), False
    for b in buf:
        if b == FLAG:
            if len(cur) > 2 and fcs(cur[:-2]) == int.from_bytes(cur[-2:], "little"):
                frames.append(bytes(cur[:-2]))
            cur, esc = bytearray(), False
        elif b == ESC:
            esc = True
        else:
            cur.append(b ^ ESC_XOR if esc else b)
            esc = False
    return frames


def unpack_uint(d, i=0):            # spinel packed unsigned int
    val, shift = 0, 0
    while i < len(d):
        val |= (d[i] & 0x7F) << shift
        i, shift = i + 1, shift + 7
        if not d[i - 1] & 0x80:
            break
    return val, i


def main():
    port = sys.argv[1]
    ser = serial.Serial(port, 115200, timeout=1.5, rtscts=False)
    ser.dtr = True                   # CONFIG_UART_LINE_CTRL=y wants DTR
    time.sleep(0.4)
    ser.reset_input_buffer()

    ok = 0
    for tid, prop in enumerate([1, 2, 5], start=1):
        ser.write(encode([0x80 | tid, CMD_PROP_VALUE_GET, prop]))
        ser.flush()
        time.sleep(0.5)
        for f in decode(ser.read(4096)):
            cmd, i = unpack_uint(f, 1)
            if cmd != CMD_PROP_VALUE_IS:
                continue
            pid, i = unpack_uint(f, i)
            body = f[i:]
            if pid == 2:
                print(f"  NCP_VERSION      : {body.rstrip(chr(0).encode()).decode(errors='replace')}")
                ok += 1
            elif pid == 1:
                major, j = unpack_uint(body); minor, _ = unpack_uint(body, j)
                print(f"  PROTOCOL_VERSION : {major}.{minor}")
                ok += 1
            elif pid == 5:
                caps, j = [], 0
                while j < len(body):
                    c, j = unpack_uint(body, j); caps.append(c)
                print(f"  CAPS             : {len(caps)} capabilities {caps[:12]}")
                ok += 1
    ser.close()
    print(f"\n{'PASS' if ok >= 2 else 'FAIL'}: {ok}/3 properties answered")
    return 0 if ok >= 2 else 1


if __name__ == "__main__":
    sys.exit(main())
