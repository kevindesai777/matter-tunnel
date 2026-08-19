#!/usr/bin/env python3
"""
Phase 5 latency harness for the VendorTunnel.

Measures end-to-end round-trip latency for signed vs unsigned commands through
a commercial hub's Thread border router.

Design notes
------------
* chip-tool runs in `interactive start` mode so one CASE session serves every
  sample. A fresh process per sample would measure mDNS discovery and session
  establishment (~1-2 s) rather than the command round trip.

* Latency is taken from chip-tool's own EM-layer timestamps - TX of
  InvokeCommandRequest to RX of InvokeCommandResponse - not from Python wall
  clock, so harness overhead is excluded.

* Conditions are INTERLEAVED, not run in blocks, so drift in ambient RF does not
  correlate with the treatment. This is the protocol requirement, and it matters:
  the effect being measured (~40 ms) is smaller than the observed spread.

* Signed samples cannot be repeated. Each needs a fresh counter and signature or
  the replay guard rejects it - correctly. This is why chip-tool's --repeat-count
  is unusable here and every signed sample is generated individually.

Usage:
  harness.py --samples 1000 --out results.csv

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import csv
import os
import re
import struct
import subprocess
import sys
import time

CT = os.path.expanduser(
    "~/ncs-door-lock/project-workspace/modules/lib/matter/examples/chip-tool/out/host/chip-tool")
STORAGE = os.path.expanduser("~/ncs-door-lock/chip-tool-storage")
TUNNEL = os.path.expanduser("~/matter-tunnel/")

CLUSTER = 0xFFF1FC02
COMMAND = 0xFFF10000
NODE = 1
ENDPOINT = 1

# chip-tool emits CSI sequences beyond colour (e.g. \x1b[0J erase) before
# the timestamp; strip the whole CSI family or the timestamp never matches.
ANSI = re.compile(r'\x1b\[[0-9;]*[A-Za-z]')
TS = re.compile(r'\[(\d{10}\.\d+)\]')


def load_keys():
    from cryptography.hazmat.primitives import serialization
    priv = serialization.load_pem_private_key(
        open(TUNNEL + "client_key.pem", "rb").read(), password=None)
    dpub = open(TUNNEL + "tunnel_pub.bin", "rb").read()
    return priv, dpub


def preimage(direction, counter, key, payload):
    return (b"MTUNv1" + bytes([direction]) + struct.pack('>I', counter)
            + struct.pack('>I', CLUSTER) + struct.pack('>I', COMMAND)
            + struct.pack('>H', len(key)) + key + payload)


class Session:
    """chip-tool interactive REPL wrapper."""

    def __init__(self):
        self.p = subprocess.Popen(
            [CT, "interactive", "start", "--storage-directory", STORAGE],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1)
        self._wait_ready()

    def _wait_ready(self, timeout=60):
        # Interactive mode prints nothing until the first command is issued, so
        # there is no readiness banner to block on - reading here would hang
        # forever. Just let the process come up; the caller issues a warm-up
        # command whose timing is discarded, which absorbs mDNS discovery and
        # CASE establishment.
        time.sleep(3)

    def invoke(self, payload_hex, timeout=30):
        """Send one command; return (rtt_ms, resp_type) or (None, None)."""
        cmd = (f'any command-by-id 0x{CLUSTER:08X} 0x{COMMAND:08X} '
               f'\'{{"0":"hex:{payload_hex}"}}\' {NODE} {ENDPOINT}\n')
        self.p.stdin.write(cmd)
        self.p.stdin.flush()

        t_tx = t_rx = None
        resp_type = None
        collecting = False
        hexbytes = []
        end = time.time() + timeout

        while time.time() < end:
            line = self.p.stdout.readline()
            if not line:
                break
            line = ANSI.sub('', line)
            m = TS.search(line)
            ts = float(m.group(1)) if m else None

            if 'InvokeCommandRequest' in line and 'Msg TX' in line and ts:
                t_tx = ts
            if 'InvokeCommandResponse' in line and 'Msg RX' in line and ts:
                t_rx = ts

            body = re.sub(r'^\[[^\]]*\]\s*\[[^\]]*\]\s*\[[^\]]*\]\s*', '', line)
            if '0x0 = [' in body:
                collecting = True
                continue
            if collecting:
                if re.search(r'\]\s*\(\d+ bytes\)', body):
                    collecting = False
                    if hexbytes:
                        resp_type = int(hexbytes[0], 16)
                    if t_tx and t_rx:
                        return (t_rx - t_tx) * 1000.0, resp_type
                    return None, resp_type
                hexbytes += re.findall(r'0x([0-9a-fA-F]{2})', body)

            # a status-only reply (no payload) still ends the exchange
            if 'CommandStatusIB' in line and t_tx and t_rx:
                return (t_rx - t_tx) * 1000.0, None

        return None, resp_type

    def close(self):
        try:
            self.p.stdin.write("quit\n")
            self.p.stdin.flush()
        except Exception:
            pass
        self.p.terminate()


def pct(vals, p):
    if not vals:
        return float('nan')
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarise(name, vals):
    if not vals:
        print(f"{name:22s} no samples")
        return
    print(f"{name:22s} n={len(vals):5d}  "
          f"p50={pct(vals,50):7.1f}  p95={pct(vals,95):7.1f}  "
          f"p99={pct(vals,99):7.1f}  min={min(vals):7.1f}  max={max(vals):7.1f} ms")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=100,
                    help="samples PER CONDITION")
    ap.add_argument("--out", default=TUNNEL + "measure/results.csv")
    ap.add_argument("--counter-start", type=int, default=100000)
    ap.add_argument("--payload", default="unlock")
    args = ap.parse_args()

    priv, dpub = load_keys()
    payload = args.payload.encode()

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    f = open(args.out, "w", newline="")
    w = csv.writer(f)
    w.writerow(["seq", "condition", "counter", "rtt_ms", "resp_type",
                "req_bytes", "unix_time"])

    s = Session()
    unsigned, signed = [], []
    counter = args.counter_start
    errors = 0

    print(f"warm-up (discarded): establishing session ...", flush=True)
    rtt, rt = s.invoke("FF" + payload.hex(), timeout=90)
    print(f"  session up (warm-up rtt={rtt if rtt is None else round(rtt,1)} ms, "
          f"type=0x{rt:02x})" if rt is not None else "  warm-up produced no reply",
          flush=True)

    print(f"\ninterleaving {args.samples} samples per condition "
          f"(signed / unsigned)\n", flush=True)

    try:
        for i in range(args.samples):
            # --- unsigned control (echo) ---
            up = "FF" + payload.hex()
            rtt, rt = s.invoke(up)
            if rtt is not None:
                unsigned.append(rtt)
                w.writerow([i, "unsigned", "", f"{rtt:.3f}", rt,
                            len(up) // 2, f"{time.time():.3f}"])
            else:
                errors += 1

            # --- signed ---
            sig = priv.sign(preimage(0x02, counter, dpub, payload))
            sp = ("02" + "%08x" % counter + "%04x" % len(payload)
                  + payload.hex() + "%04x" % len(sig) + sig.hex())
            rtt, rt = s.invoke(sp)
            if rtt is not None and rt == 0x03:
                signed.append(rtt)
                w.writerow([i, "signed", counter, f"{rtt:.3f}", rt,
                            len(sp) // 2, f"{time.time():.3f}"])
            else:
                errors += 1
                if rt is not None and rt != 0x03:
                    print(f"  ! sample {i}: unexpected response type 0x{rt:02x}")
            counter += 1

            if (i + 1) % 25 == 0:
                print(f"  {i+1}/{args.samples}  "
                      f"unsigned n={len(unsigned)} signed n={len(signed)} "
                      f"errors={errors}", flush=True)
    except KeyboardInterrupt:
        print("\ninterrupted - writing what we have")
    finally:
        s.close()
        f.close()

    print()
    summarise("unsigned (echo)", unsigned)
    summarise("signed", signed)
    if unsigned and signed:
        d = pct(signed, 50) - pct(unsigned, 50)
        print(f"\ndelta p50 = {d:+.1f} ms")
        print(f"request bytes: unsigned {len(payload)+1} -> "
              f"signed {1+4+2+len(payload)+2+64} "
              f"(+{(1+4+2+len(payload)+2+64)-(len(payload)+1)} B)")
    print(f"errors: {errors}")
    print(f"csv: {args.out}")


if __name__ == "__main__":
    main()
