# Measurement

```
harness.py       Matter conditions, driving chip-tool in interactive mode
harness_ble.py   direct-BLE condition, run from the Raspberry Pi
analyse.py       regenerates every published figure from the raw CSVs
data/            the raw datasets and the harness run logs
```

Run `python3 analyse.py` to reproduce the results tables in the top-level README.
No figure in that README is transcribed by hand.

## The datasets

| File | n | What it is |
|---|---:|---|
| `results-smartthings.csv` | 1000 | Matter over Thread through a commercial hub. The treatment |
| `results-otbr-final.csv` | 1000 | Matter over Thread through our own border router. **The control**, and the reportable OTBR run |
| `results-otbr.csv` | 1000 | The same control at −85 dBm instead of −26 dBm. **Retained as evidence, not as a result**; see below |
| `results-ble.csv` | 1000 | Direct BLE, no Matter, no Thread, no hub. The transport floor |
| `pilot*.csv` | 5–20 | Pilot runs used to size the protocol; not reported |

Columns are `seq, condition, counter, rtt_ms, resp_type, req_bytes, unix_time`.
`resp_type` is the first byte of the response envelope: `0xFF` unsigned echo,
`0x03` signed. `seq` pairs an unsigned and a signed sample from the same iteration,
which is what makes the paired analysis in `analyse.py` possible.

## Why the weak-link run is kept

`results-otbr.csv` and `results-otbr-final.csv` differ in **nothing but the physical
distance between the device and the border router's radio**. Same firmware, same
border router, same harness, same protocol.

| | RSSI | p50 | p95 | p99 | max | effect size |
|---|---|---:|---:|---:|---:|---:|
| `results-otbr.csv` | −85 dBm | 96 ms | 128 ms | 179 ms | **1973 ms** | 0.8σ |
| `results-otbr-final.csv` | −26 dBm | 90 ms | 95 ms | 115 ms | 300 ms | 4.9σ |

Link margin alone moves p50 by 6 ms and p99 by 64 ms, and inflates the unsigned
standard deviation from 8.3 ms to 63.3 ms, enough to bury a real 40 ms effect under
noise and report it as marginal. The weak-link dataset is in the repository because
the finding it supports is a methodological one: **an RF condition nobody wrote down
can decide whether an effect is detectable at all.**

## Protocol notes

- **Interleaved, not blocked.** One unsigned and one signed sample per iteration, so
  drift in ambient RF cannot correlate with the treatment. The effect is smaller than
  the spread, which is exactly the regime where blocking goes wrong.
- **Timestamps come from chip-tool's EM layer**, TX of `InvokeCommandRequest` to RX
  of `InvokeCommandResponse`, not from Python wall clock, so harness overhead stays
  outside the measurement.
- **One CASE session serves the whole run.** A fresh process per sample would measure
  mDNS discovery and session establishment, one to two seconds, rather than the
  command round trip.
- **Signed samples cannot be repeated.** Each needs a fresh counter and a fresh
  signature, or the replay guard rejects it, correctly. Start `--counter-start` above
  the device's current floor.
- **Percentiles, not means, are the headline.** For a lock the tail is the interesting
  part. `analyse.py` reports both, plus the standard deviation the effect size is
  measured against.
