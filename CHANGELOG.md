# Changelog

## 1.0.0 (2026-08-19)

First public release, archived with a DOI. This is the artifact accompanying a
measurement study of commercial Matter ecosystems and of removing the hub from the
device trust boundary.

### The component

- Manufacturer-specific Matter cluster `0xFFF1FC02` carrying an opaque bidirectional
  byte tunnel: `TunnelRequest` / `TunnelResponse`, `octet_string<512>` each way.
  **+612 B flash, 0 B RAM** for the transport alone.
- Application-layer signing in both directions, Ed25519 via PSA. Signature preimage
  binds a domain tag, direction, counter, cluster ID, command ID and a
  length-prefixed device key alongside the payload; each field verified bound by
  tampering with it.
- Anti-replay by monotonic counter with a *reserved ceiling* persisted before
  acceptance, so a reboot can lose an acceptance but never a reservation.
- Tunnel key bound to the device's own Matter DAC, validated DAC → PAI → PAA against
  public CSA roots. No out-of-band channel and no vendor backend.
- Client key registration gated on physical presence.
- Two transport adapters, the Matter vendor cluster and a direct BLE link, over one
  shared transport-agnostic core, so the measured difference between them is
  attributable to transport alone.
- Complete security layer: **+19,652 B flash, +3,840 B RAM**.

### Measurement

- Interleaved harness for the Matter and BLE conditions, and `analyse.py`, which
  regenerates every published figure from the raw CSVs.
- Three datasets at n=1000 with zero errors, plus a fourth retained as evidence for
  the link-margin finding.
- On-device DWT cycle-counter benchmark for the primitives.

### Notes for anyone reproducing

- `examples/lock/prj.conf` now states the non-sleepy MED configuration explicitly.
  It previously came from an overlay applied to a build directory and recorded in no
  tracked file, so a fresh build produced a *sleepy* device with a ~2.3 s polling
  floor, not the firmware the measurements were taken on. A clean build from this
  repository reproduces the measured configuration and footprint.
- `zap/generated/zap-generated/` is not tracked; regenerate it with the three
  commands in `docs/REPRODUCING.md`.
- `client_key.pem` is not and will not be tracked. Generate your own.

### Known gaps

Stated in full in [SECURITY.md](SECURITY.md). In brief: the swap-attack fingerprint
is specified and not implemented, one counter space means one client, and payloads
are authenticated but not encrypted.
