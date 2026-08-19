#!/usr/bin/env python3
"""
Condition 0 - BLE transport floor for the VendorTunnel.

Carries the SAME envelope bytes as measure/harness.py, signed by the SAME client
key, verified by the SAME on-device core (tunnel::ProcessRequest) - but over a
direct BLE link with no Matter, no Thread and no border router in the path. The
difference between this and condition 1 is therefore attributable to transport.

Runs on the Raspberry Pi: BLE from Python on macOS aborts under TCC because the
calling binary has no NSBluetoothAlwaysUsageDescription.

Measurement notes
-----------------
* Conditions are INTERLEAVED, matching harness.py, so RF drift cannot correlate
  with the treatment.

* RTT is client-side wall clock (write -> notification). Unlike harness.py, which
  reads chip-tool's internal EM timestamps, this necessarily includes BlueZ and
  D-Bus overhead. The signed-minus-unsigned DELTA cancels that overhead and is
  directly comparable across conditions; the absolute floor is an upper bound.

* The replay counter space is shared with the Matter transport (one counter, one
  client - see tunnel_replay.h), so --counter-start must exceed the floor left by
  any earlier run.

SPDX-License-Identifier: Apache-2.0
"""
import argparse, asyncio, csv, os, struct, sys, time

from bleak import BleakClient, BleakScanner

NUS_SVC = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_RX  = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"   # client -> device (write)
NUS_TX  = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"   # device -> client (notify)

CLUSTER = 0xFFF1FC02
COMMAND = 0xFFF10000
HOME    = os.path.expanduser("~")


def preimage(direction, counter, key, payload):
    return (b"MTUNv1" + bytes([direction]) + struct.pack('>I', counter)
            + struct.pack('>I', CLUSTER) + struct.pack('>I', COMMAND)
            + struct.pack('>H', len(key)) + key + payload)


def load_keys(keydir):
    from cryptography.hazmat.primitives import serialization
    priv = serialization.load_pem_private_key(
        open(os.path.join(keydir, "client_key.pem"), "rb").read(), password=None)
    dpub = open(os.path.join(keydir, "tunnel_pub.bin"), "rb").read()
    return priv, dpub


def pct(vals, p):
    if not vals:
        return float('nan')
    s = sorted(vals); k = (len(s) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def summarise(name, vals):
    if not vals:
        print(f"{name:22s} no samples"); return
    print(f"{name:22s} n={len(vals):5d}  p50={pct(vals,50):7.1f}  p95={pct(vals,95):7.1f}  "
          f"p99={pct(vals,99):7.1f}  min={min(vals):7.1f}  max={max(vals):7.1f} ms")


class Link:
    """
    Resolves the two NUS characteristics ONCE and then passes the objects, not
    their UUID strings.

    bleak looks a UUID string up through `client.services` on every call, and
    that property raises BleakError("Service Discovery has not been performed
    yet") whenever its cached service collection is not populated - which was
    observed here *while `is_connected` still reported True*, so it is not a
    reliable liveness signal. Handing over the characteristic object skips the
    lookup entirely, which is both more robust and marginally cheaper per
    sample.
    """

    def __init__(self, client):
        self.c = client
        self.q = asyncio.Queue()
        self.rx = None
        self.tx = None

    async def start(self):
        svcs = self.c.services
        self.rx = svcs.get_characteristic(NUS_RX)
        self.tx = svcs.get_characteristic(NUS_TX)
        if self.rx is None or self.tx is None:
            raise RuntimeError("NUS characteristics not found - is this the right device?")
        await self.c.start_notify(self.tx, lambda _s, d: self.q.put_nowait((time.perf_counter(), bytes(d))))

    async def exchange(self, payload: bytes, timeout=10.0):
        while not self.q.empty():
            self.q.get_nowait()
        t0 = time.perf_counter()
        await self.c.write_gatt_char(self.rx, payload, response=False)
        try:
            t1, data = await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None, None
        return (t1 - t0) * 1000.0, data[0] if data else None


async def run(args):
    priv, dpub = load_keys(args.keydir)
    payload = args.payload.encode()

    print(f"scanning for '{args.name}' ...", flush=True)
    dev = await BleakScanner.find_device_by_name(args.name, timeout=25.0)
    if dev is None:
        print(f"  device '{args.name}' not found - is it advertising?"); return 1
    print(f"  found {dev.address}", flush=True)

    async with BleakClient(dev) as c:
        mtu = getattr(c, "mtu_size", None)
        print(f"  connected, ATT MTU = {mtu}", flush=True)
        if mtu and mtu < 100:
            print(f"  ! MTU {mtu} is too small for a signed envelope; results would "
                  f"measure fragmentation", flush=True)

        link = Link(c)
        await link.start()

        os.makedirs(os.path.dirname(args.out), exist_ok=True)
        f = open(args.out, "w", newline="")
        w = csv.writer(f)
        w.writerow(["seq", "condition", "counter", "rtt_ms", "resp_type", "req_bytes", "unix_time"])

        unsigned, signed = [], []
        counter = args.counter_start
        errors = 0

        rtt, rt = await link.exchange(b"\xFF" + payload, timeout=20)
        print(f"  warm-up rtt={None if rtt is None else round(rtt,1)} ms "
              f"type={'none' if rt is None else hex(rt)}", flush=True)

        print(f"\ninterleaving {args.samples} samples per condition (signed / unsigned)\n", flush=True)
        try:
            for i in range(args.samples):
                up = b"\xFF" + payload
                rtt, rt = await link.exchange(up)
                if rtt is not None:
                    unsigned.append(rtt)
                    w.writerow([i, "unsigned", "", f"{rtt:.3f}", rt, len(up), f"{time.time():.3f}"])
                else:
                    errors += 1

                sig = priv.sign(preimage(0x02, counter, dpub, payload))
                sp = (b"\x02" + struct.pack('>I', counter) + struct.pack('>H', len(payload))
                      + payload + struct.pack('>H', len(sig)) + sig)
                rtt, rt = await link.exchange(sp)
                if rtt is not None and rt == 0x03:
                    signed.append(rtt)
                    w.writerow([i, "signed", counter, f"{rtt:.3f}", rt, len(sp), f"{time.time():.3f}"])
                else:
                    errors += 1
                    if rt is not None and rt != 0x03:
                        print(f"  ! sample {i}: unexpected response type 0x{rt:02x}", flush=True)
                counter += 1

                if (i + 1) % 25 == 0:
                    print(f"  {i+1}/{args.samples}  unsigned n={len(unsigned)} "
                          f"signed n={len(signed)} errors={errors}", flush=True)
        finally:
            f.close()

        print()
        summarise("unsigned (echo)", unsigned)
        summarise("signed", signed)
        if unsigned and signed:
            print(f"\ndelta p50 = +{pct(signed,50) - pct(unsigned,50):.1f} ms")
        print(f"request bytes: unsigned {1+len(payload)} -> signed {9+len(payload)+64}")
        print(f"errors: {errors}")
        print(f"csv: {args.out}")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", type=int, default=100, help="samples PER CONDITION")
    ap.add_argument("--out", default=os.path.join(HOME, "ble-results.csv"))
    ap.add_argument("--counter-start", type=int, default=400000)
    ap.add_argument("--payload", default="unlock")
    ap.add_argument("--name", default="MatterLock")
    ap.add_argument("--keydir", default=HOME)
    return asyncio.run(run(ap.parse_args()))


if __name__ == "__main__":
    sys.exit(main())
