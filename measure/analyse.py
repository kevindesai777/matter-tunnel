#!/usr/bin/env python3
"""
Recompute every published figure from the raw datasets.

There is no hand-carried number in the paper that this script cannot
reproduce from `measure/data/*.csv`. That is the point of it: a results table
transcribed by hand drifts from the data behind it, and the drift is invisible.

The experiment is PAIRED — the harness interleaves one unsigned and one signed
sample per iteration, precisely so that drift in ambient RF cannot correlate
with the treatment. Both the unpaired and the paired interval are printed. The
unpaired difference of means is reported as primary because it makes no
assumption about the pairing surviving dropped samples; the paired interval is
the tighter cross-check, and the two agreeing is itself evidence that the
interleaving worked.

  analyse.py                                        # all datasets
  analyse.py data/results-ble.csv --interval 15     # one, with the BLE
                                                    # connection interval

SPDX-License-Identifier: Apache-2.0
"""
import csv
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# resp_type is the first byte of the response envelope.
RESP_UNSIGNED = 0xFF
RESP_SIGNED = 0x03


def pct(vals, p):
    """Linear-interpolated percentile; matches the harness's own definition."""
    if not vals:
        return float("nan")
    s = sorted(vals)
    k = (len(s) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(s) - 1)
    return s[lo] + (s[hi] - s[lo]) * (k - lo)


def mean(v):
    return sum(v) / len(v)


def sd(v):
    """Sample standard deviation (n-1)."""
    if len(v) < 2:
        return float("nan")
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


def load(path):
    rows = []
    with open(path, newline="") as f:
        for r in csv.DictReader(f):
            if not r.get("rtt_ms"):
                continue
            rows.append({
                "seq": int(r["seq"]),
                "condition": r["condition"],
                "rtt": float(r["rtt_ms"]),
                "resp": int(r["resp_type"]) if r["resp_type"] else None,
                "bytes": int(r["req_bytes"]) if r["req_bytes"] else None,
            })
    return rows


def report(path, interval=None):
    rows = load(path)
    unsigned = [r for r in rows if r["condition"] == "unsigned"]
    signed = [r for r in rows if r["condition"] == "signed"]

    # An error is a sample whose response envelope is not the expected type.
    # A dropped sample never reaches the CSV at all, so it shows up as a short
    # count against the requested n rather than as a bad row.
    bad_u = [r for r in unsigned if r["resp"] != RESP_UNSIGNED]
    bad_s = [r for r in signed if r["resp"] != RESP_SIGNED]

    u = [r["rtt"] for r in unsigned]
    s = [r["rtt"] for r in signed]

    print(f"\n=== {os.path.relpath(path, HERE)} ===")
    if not u or not s:
        print("  incomplete dataset")
        return

    hdr = f"{'':10s} {'n':>5s} {'p50':>8s} {'p95':>8s} {'p99':>8s} " \
          f"{'min':>8s} {'max':>8s} {'mean':>8s} {'sd':>7s}"
    print(hdr)
    for name, v in (("unsigned", u), ("signed", s)):
        print(f"{name:10s} {len(v):5d} {pct(v,50):8.1f} {pct(v,95):8.1f} "
              f"{pct(v,99):8.1f} {min(v):8.1f} {max(v):8.1f} "
              f"{mean(v):8.2f} {sd(v):7.2f}")

    req_u = {r['bytes'] for r in unsigned if r['bytes']}
    req_s = {r['bytes'] for r in signed if r['bytes']}
    print(f"\n  wire size          : {sorted(req_u)} -> {sorted(req_s)} bytes")
    print(f"  errors             : {len(bad_u)} unsigned, {len(bad_s)} signed")

    # --- unpaired difference of means, 95% CI (normal approximation; n=1000)
    d = mean(s) - mean(u)
    se = math.sqrt(sd(s) ** 2 / len(s) + sd(u) ** 2 / len(u))
    half = 1.96 * se
    print(f"\n  signing cost       : {d:+.2f} ms   "
          f"95% CI [{d-half:.2f}, {d+half:.2f}]   (unpaired, n={len(u)}/{len(s)})")
    print(f"  effect size        : {d / sd(u):.1f} sigma against unsigned spread "
          f"(sd = {sd(u):.2f} ms)")

    # --- paired difference, the design the harness actually implements
    by_seq_u = {r["seq"]: r["rtt"] for r in unsigned}
    by_seq_s = {r["seq"]: r["rtt"] for r in signed}
    pairs = [by_seq_s[k] - by_seq_u[k] for k in by_seq_u.keys() & by_seq_s.keys()]
    if len(pairs) > 1:
        pd_, psd = mean(pairs), sd(pairs)
        phalf = 1.96 * psd / math.sqrt(len(pairs))
        print(f"  paired cross-check : {pd_:+.2f} ms   "
              f"95% CI [{pd_-phalf:.2f}, {pd_+phalf:.2f}]   (n={len(pairs)} pairs)")

    print(f"  p50 delta          : {pct(s,50) - pct(u,50):+.1f} ms")
    print(f"  samples over 300ms : {sum(1 for x in u + s if x > 300)}")

    # --- Quantisation check, only when a link-layer interval is supplied.
    #
    # This must NOT be run speculatively. Binning any distribution whose spread
    # is comparable to the bin width will concentrate it in one bin, so the test
    # "proves" quantisation for data that has none - it fires on the Matter
    # conditions, which have no connection interval at all. The interval is a
    # property of the link that only the operator knows, so it has to be passed
    # in, and the result is only meaningful for the transport it describes.
    if interval:
        for name, v in (("unsigned", u), ("signed", s)):
            bins = {}
            for x in v:
                bins[round(x / interval)] = bins.get(round(x / interval), 0) + 1
            top, count = max(bins.items(), key=lambda kv: kv[1])
            print(f"  quantisation ({name:8s}): {count}/{len(v)} samples in the "
                  f"{top * interval:.0f} ms bin = {top} x {interval:.0f} ms "
                  f"connection events")


def main():
    args = sys.argv[1:]
    interval = None
    if "--interval" in args:
        i = args.index("--interval")
        interval = float(args[i + 1])
        del args[i:i + 2]
    if args:
        paths = [a if os.path.isabs(a) else os.path.join(HERE, a) for a in args]
    else:
        d = os.path.join(HERE, "data")
        paths = [os.path.join(d, f) for f in sorted(os.listdir(d))
                 if f.endswith(".csv")]
    for p in paths:
        report(p, interval)
    print()


if __name__ == "__main__":
    main()
