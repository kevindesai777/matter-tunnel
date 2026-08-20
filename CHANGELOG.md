# Changelog

## 1.1.0 (2026-08-20)

Makes the reported cost reproducible. No change to the shipped configuration: a default
build of 1.1.0 is byte-identical to 1.0.0 at 797,600 B of flash and 167,452 B of RAM.

### Added

- `examples/light_bulb`, the second device type. Same component, untouched: a different
  device class (On/Off and Level Control), a base application 21 KB larger, and a Full
  Thread Device rather than a Minimal End Device. Every flash delta is identical to the
  lock's, +20,212 B complete. RAM differs by 64 B of section alignment; the component's
  static RAM is 2,273 B in both images symbol for symbol.
- `lib/tunnel/Kconfig`, so the component carries its own configuration instead of each
  example redeclaring it. `CONFIG_TUNNEL_CRYPTO_BENCHMARK` moved here from
  `examples/lock/Kconfig`.
- `CONFIG_TUNNEL_SECURITY` and `CONFIG_TUNNEL_CLIENT_AUTH`, both defaulting to `y`.
  Turning them off builds the transport-only and unauthenticated-request stages of the
  cost table. Neither reduced build is for deployment.
- `examples/lock/footprint-stage2.conf` and `footprint-stage3.conf`, which select those
  stages, and a table of expected figures in `docs/REPRODUCING.md`.

### Changed (tooling)

`tools/fabric-unreachable-test.sh` assumed one ecosystem holding one fabric. It now:

- takes several fabric identifiers in `ECO_VIDS` and reports a verdict per fabric, so an
  ecosystem that releases one credential and keeps another is distinguishable from one that
  releases all or none;
- namespaces its output with `LABEL`, because without it a second run overwrites the evidence
  behind an already-published result;
- matches on Compressed FabricId rather than VendorId. The device emits VendorId last on the
  line and a concurrent log line can interleave and truncate it; a truncated record reads
  exactly like an absent fabric, which would report a retained credential as released;
- cross-checks the boot-record count against `CommissionedFabrics` and declines to issue a
  verdict when the two instruments disagree;
- no longer claims a fabric is orphaned after a 60-second settle. That only rules out a short
  deferral, and one ecosystem is known to defer for tens of seconds.

### Corrected

The 1.0.0 figures below were measured on 2026-08-17, before the core was extracted into
`tunnel::ProcessRequest` for the second transport adapter. That refactor shipped in 1.0.0
but the numbers were not re-derived against it. Rebuilt from this tree:

| | 1.0.0 said | Actual |
|---|---:|---:|
| Tunnel cluster, transport only | +612 B flash, 0 B RAM | **+808 B flash, +576 B RAM** |
| Complete security layer | +19,652 B flash, +3,840 B RAM | **+19,404 B flash, +3,008 B RAM** |

512 B of the transport stage's RAM is the response buffer `ProcessRequest` writes into. It is
a cost of the transport-agnostic core. 1.0.0 counted it against the security layer, because
the Matter-only handler it replaced had built its response inline.

### Changed

- `zap/add_tunnel_cluster.py` chose its endpoint by looking for a Door Lock cluster, which
  contradicted the device independence it exists to support. It now takes the first
  application endpoint, skipping the root device. Behaviour is unchanged for the lock.

### Fixed

- `EnsureReplay()` initialised the client key store, so the replay counter did not
  compile without client authentication. The two subsystems were coupled by a lazy-init
  shortcut rather than by design.
- `docs/REPRODUCING.md` gave one flash figure without saying that the boot banner carries
  the git description, so a build from the Zenodo archive is 4 B larger than one from a
  clone. Both are correct.
- `README.md` paired a +3,840 B RAM delta with a 167,452 B image, which did not reconcile
  against the base.

## 1.0.0 (2026-08-19)

First public release, archived with a DOI. This is the artifact accompanying a
measurement study of commercial Matter ecosystems and of removing the hub from the
device trust boundary.

### The component

- Manufacturer-specific Matter cluster `0xFFF1FC02` carrying an opaque bidirectional
  byte tunnel: `TunnelRequest` / `TunnelResponse`, `octet_string<512>` each way.
  **+612 B flash, 0 B RAM** for the transport alone. ⚠️ Corrected in 1.1.0.
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
- Complete security layer: **+19,652 B flash, +3,840 B RAM**. ⚠️ Corrected in 1.1.0.

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
