# matter-tunnel

A portable manufacturer-specific Matter cluster carrying an opaque bidirectional
byte tunnel, plus an application-layer signing layer that authenticates payloads end
to end. An intermediating Matter hub relays bytes it can neither interpret nor forge,
so the hub keeps its role as a router and loses its role as an authority.

Built clean-room against the public Matter specification and SDK. The component
carries no device-type semantics and drops into any Matter application. The door lock
here is one example of that.

[![License](https://img.shields.io/badge/license-Apache--2.0-blue.svg)](LICENSE)
<!-- DOI badge is added when the first release is archived. -->

---

## Why

Matter grants every commissioned hub genuine authority over the device it controls.
A hub can *operate* a lock, not merely forward a user's request. Certification does
not narrow this: it verifies that a *device* conforms to the specification, never
that a *hub* merits the authority the specification hands it.

Two measurements on this bench make the problem concrete:

- **Network isolation does not revoke authority.** With the device moved entirely
  onto a Thread network under our own control, a commercial hub kept operating it.
  Our own border router relayed its traffic and advertised the hub's fabric record
  onto the LAN. Matter authority is an IP-layer relationship; Thread membership is
  link-layer. They are independent, so topology cannot revoke authority. Only fabric
  removal or a factory reset can.
- **Removal while unreachable is deferred, and the UI reports completion first.**
  With the IP path cut, removing the device in the ecosystem app made it vanish from
  the app immediately, while the hub still held working operational credentials and
  used them about 40 seconds after the device came back. Ours was short only because
  the border router was restored promptly. A device offline for a week leaves the hub
  in authority for that week, with nothing in the UI indicating a pending revocation.

If topology cannot remove a hub from the trust boundary, cryptography has to. That
is what this component is for.

**The mechanism is not novel and this repository does not claim it is.** Putting
security mechanisms into Matter manufacturer-specific clusters is the subject of
Mangar, Chandler, Pierson and Kotz, *"Enabling Research Extensions in Matter via
Custom Clusters"* (NDSS SDIoTSec 2026). End-to-end authentication through untrusted
intermediaries is older still: JEDI (USENIX Security 2019) and OSCORE (RFC 8613).
What is contributed here is a working implementation on constrained silicon, the
measured cost of running it, and measurements of what commercial ecosystems actually
do with the authority they hold.

---

## Measured results

All latency figures: n=1000 per condition, interleaved rather than blocked so RF
drift cannot correlate with the treatment, zero errors, on nRF52840.
`measure/analyse.py` regenerates every number below from the raw data. Nothing in
these tables is transcribed by hand.

### What signing costs

| Transport | unsigned p50 | signed p50 | signing cost (mean) | 95% CI |
|---|---:|---:|---:|---|
| Matter over Thread, our own border router | 90.0 ms | 130.0 ms | **+40.35 ms** | [39.64, 41.07] |
| Matter over Thread, commercial hub | 103.0 ms | 144.0 ms | **+41.35 ms** | [40.69, 42.00] |
| Direct BLE, no Matter/Thread/hub | 29.0 ms | 60.7 ms | +32.42 ms ⚠️ | [31.78, 33.06] |

**Signing cost is invariant to the transport carrying it.** The two Matter confidence
intervals overlap, and the border-router interval *contains* the isolated on-device
benchmark of 39.9 ms measured with the Cortex-M DWT cycle counter. That is three
instruments, and the strongest two agree to within the width of a confidence interval.

⚠️ **Do not quote the BLE delta as the cost of signing.** The 15 ms connection
interval quantises it: 977 of 1000 unsigned samples land in the 30 ms bin (two
connection events) and 945 of 1000 signed samples in the 60 ms bin (four). The
link-layer schedule sets the latency, not the processing, so an operation of about 40 ms appears
as exactly two extra events. `measure/analyse.py --interval 15` prints this.

### The hub is not the bottleneck

| Layer | unsigned p50 | increment | share of the 103 ms path |
|---|---:|---:|---:|
| Direct BLE link floor | 29.0 ms | — | 28.2% |
| Matter over Thread via our own border router | 90.0 ms | +61.0 ms | 59.2% |
| A commercial hub instead of our border router | 103.0 ms | +13.0 ms | 12.6% |

A commercial hub costs about 13 ms, roughly an eighth of the path. Matter over Thread
with a border router you control costs about 61 ms, some 4.7× more. Removing the hub
from the device's trust boundary is affordable because the hub was never the expensive
part. Without the control condition the 103 ms would have been reported as hub
latency, wrong by a factor of five.

⚠️ The 13 ms is an upper bound on hub overhead, not a clean measurement of it. The
commercial hub's link margin was never characterised at comparable distance, and this
bench showed link margin alone moving p50 by 6 ms and p99 by 64 ms.

### What it costs on the device

| | Flash | RAM |
|---|---:|---:|
| Tunnel cluster, transport only | **+612 B** | **0 B** |
| Complete security layer | **+19,652 B** | **+3,840 B** |
| Resulting image | 797,604 B of 974,698 B (81.83%) | 167,452 B of 256 KB (63.88%) |

Wire overhead is 7 → 79 bytes. The identity bundle is 1060 B, fetched once at client
setup in four chunks, never per command. About 90% of it is X.509, which makes key
distribution here a *certificate-transport* problem rather than a key problem.

### Choose Ed25519, and leave CryptoCell off

Cortex-M DWT cycle counter, n=20:

| Configuration | P-256 sign | P-256 verify | Ed25519 sign | Ed25519 verify | Flash |
|---|---:|---:|---:|---:|---:|
| Oberon software | 31.3 ms | 84.6 ms | **16.7 ms** | **23.2 ms** | 81.54% |
| + CryptoCell CC310 | 19.4 ms | 37.2 ms | *unavailable* | *unavailable* | 85.29% |

Ed25519 in software beats CC310-accelerated P-256 on both operations. Enabling CC310
makes Ed25519 unavailable altogether: the CC3XX driver claims ECC key operations and
does not implement Twisted Edwards, so PSA returns `PSA_ERROR_NOT_SUPPORTED`. And
CC310 costs about 36 KB of flash to get there. Ed25519 is also far tighter in
distribution, 16.67 to 16.73 ms against P-256's 16.8 to 55.7 ms from rejection-sampled
nonces, which matters when the figure being reported is p99.

---

## Layout

```
lib/tunnel/         the component: transport-agnostic core, signing, replay,
                    identity, client-key registration, two transport adapters
zap/                the cluster definition, and the script that derives an
                    application data model from a stock one
examples/lock/      a thin example application; compiles the upstream Nordic door
                    lock in place rather than copying it, so the only new source
                    in the build is the component itself
measure/            the interleaved harness, the analysis script, and the raw
                    datasets behind every number above
tools/              border-router provisioning, RCP self-test, device console
                    capture, and the fabric-retention experiment
evidence/           device console captures backing each claim, and test vectors
docs/               REPRODUCING.md: build, flash, measure, from scratch
```

### The component

| File | What it is |
|---|---|
| `tunnel_core.h` | Transport-agnostic entry point. Matter and BLE both call `ProcessRequest()`, so the signing path is *the same code* in both conditions, which is what makes the measured difference attributable to transport alone. |
| `tunnel_server.cpp` | The Matter vendor-cluster command handler. |
| `tunnel_ble.cpp` | Direct-BLE adapter, for the transport floor. |
| `tunnel_crypto.cpp` | PSA-backed sign/verify, so the algorithm is a build-time choice. |
| `tunnel_envelope.cpp` | Signature preimage construction and field binding. |
| `tunnel_replay.cpp` | Reserved-ceiling anti-replay counter. |
| `tunnel_identity.cpp` | Binds the tunnel key to the device's Matter DAC. |
| `tunnel_client_key.cpp` | Client key registration, gated on physical presence. |
| `tunnel_bench.cpp` | On-device DWT sign/verify benchmark. |

---

## The cluster

Defined against **Matter Core Specification §7.21** (Manufacturer Specific
Extensions). Note §7.21, not §7.19, which was correct only through Matter 1.3.

| | |
|---|---|
| Cluster ID | `0xFFF1FC02` (vendor `0xFFF1` test VID · cluster `0xFC02`) |
| `TunnelRequest` | client → server, `0xFFF10000`, `octet_string<512> payload` |
| `TunnelResponse` | server → client, `0xFFF10001`, `octet_string<512> payload` |

`0xFC01` is deliberately avoided: it is used by the `NordicDevKit` cluster in the
nRF Connect SDK `manufacturer_specific` sample, this definition's structural
reference. Avoiding it keeps both loadable in one ZCL database.

The 512-byte ceiling is a build-time choice, not a protocol constant. It must hold
the application payload plus authentication overhead (for Ed25519, a 64-byte
signature plus counter and binding fields). Larger values cost RAM in message
buffers, and flash is the binding constraint on nRF52840.

A `chip-tool` built from the **unmodified** Matter SDK drives the cluster, invoking
by raw identifier, so any specification-conformant client can drive it, not only one
built from this definition:

```
chip-tool any command-by-id 0xFFF1FC02 0xFFF10000 '{"0":"hex:FF48656c6c6f"}' 1 1
```

---

## How the payload is authenticated

**Preimage.** `"MTUNv1" ‖ direction ‖ counter ‖ cluster_id ‖ command_id ‖ len ‖ device_key ‖ payload`

Signing the payload alone would leave a signed message redirectable to a different
command, a different cluster or a different device, and replayable later. So the
preimage binds all of them: the domain tag prevents cross-protocol signature reuse,
`direction` stops a response being replayed as a request, and the device key is
length-prefixed to remove concatenation ambiguity. We checked every field by
tampering with it, one at a time: altered counter, flipped direction, tampered
payload, substituted device key. The device rejects each one.

**Anti-replay: a monotonic counter with a reserved ceiling.** Challenge-response would
cost an extra round trip, measured at about 250 ms, some six times the entire signing
cost, and would inflate the very latency being attributed to signing. But the naive
counter is unsafe: persisting "highest seen" makes the stored value *regress* after a
reboot that follows unpersisted acceptances, reopening the replay window. Instead the
device persists a reserved ceiling (`ceiling + 64`) *before* accepting, and keeps the
floor in RAM. The floor never moves backwards across a reboot, and flash is written
once per 64 commands. A reset can lose an acceptance, which is safe. It cannot lose a
reservation, which would not be. Resync needs no extra exchange: a rejection returns
the device's floor.

**Key distribution: bind the tunnel key to Matter's own attestation.** The device
mints a tunnel keypair and signs it with its DAC key, emitting
`{tunnel_pubkey, Sign_DAC(version‖alg‖pubkey), DAC, PAI}`. The client validates
DAC → PAI → PAA against public CSA roots. The hub holds no DAC private key, so it
cannot forge this, which is what makes it safe to carry the bundle through the
hub-relayed tunnel itself. No out-of-band channel, no vendor backend and no admin
rights, which matters because the intended audience is small makers.

**Identity is bound to a device-owned key, never to the Matter `NodeId`.** Measured
here: removing a device from an ecosystem and re-adding it preserves the fabric but
mints a *new* operational NodeId. Any scheme keyed on NodeId therefore breaks
*silently* on a re-pair. That is a nasty failure mode, because the device keeps
working until someone re-adds it.

**Read [SECURITY.md](SECURITY.md) before building on this.** The swap attack is
specified and not implemented; one counter space means one client; payloads are
authenticated but not encrypted.

---

## Reproducing

Full step-by-step in **[docs/REPRODUCING.md](docs/REPRODUCING.md)**: toolchain,
data-model generation, build, flash, border router, and each measurement condition.

### Pinned versions

| | |
|---|---|
| nRF Connect SDK | `v3.4.0` LTS (`v3.4.0-99553055607b`) |
| Zephyr | `v4.4.0-bf801e4e3d19` |
| **Matter SDK commit** | **`1206d45da3271670e676f2371670a30154d4429e`** |
| OpenThread commit (RCP) | `0fe68ff23527e8bb9a9821ca96a255fccfbb44a7` |
| `ot-br-posix` commit | `295ded43f7224fe62810559eb096e5397d2364f1` |
| Base application | nRF Door Lock and Access Control Add-on, `v1.1.0-4b8c89167696` |
| Board | nRF52840-DK (PCA10056); RCP on nRF52840 Dongle (PCA10059) |

The Matter SDK commit, not the SDK tag, is the meaningful reproducibility pin, and it
is identical in the `/opt` v3.4.0 tree and in the add-on workspace.

### Put the measured configuration in a tracked file

A build directory is not a record. If a measured artifact's configuration is not in a
tracked file, the measurement is not reproducible, and deleting a build directory
silently changes the device under test.

We learned that here. An earlier version of this work measured a build whose
sleepy-end-device setting came from a config overlay applied to a build directory and
recorded in no tracked file. A fresh build from the repository produced a *sleepy*
device with a polling floor of about 2.3 s, not the firmware the measurements were taken on.
Both options are now written explicitly into `examples/lock/prj.conf`, and a clean
build from this repository reproduces the measured configuration and footprint.

---

## Citing

See [CITATION.cff](CITATION.cff). Each release is archived with a DOI; cite the
version you used.

---

## Licence

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

`examples/lock/` retains Nordic Semiconductor's configuration files under
`LicenseRef-Nordic-5-Clause` with their original headers. Everything under
`lib/tunnel/`, `zap/`, `measure/` and `tools/` is Apache-2.0.
