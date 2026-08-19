# Evidence

Device console captures and test vectors backing the claims in the top-level README.
These are raw captures, kept as evidence rather than as documentation.

## Redaction

Globally routable IPv6 addresses in these captures carried this bench's
ISP-assigned prefix. That prefix has been replaced throughout with the RFC 3849
documentation prefix `2001:db8::/32`, preserving the subnet and interface
identifier so distinct hosts remain distinguishable. Mesh-local (`fd…`) addresses
are locally generated and are unmodified.

## Device logs

| File | What it shows |
|---|---|
| `primitive-bench-oberon.log` | DWT cycle-counter benchmark, Oberon software: Ed25519 16.7 ms sign / 23.2 ms verify against P-256's 31.3 / 84.6 |
| `primitive-bench-cc310.log` | The same benchmark with CryptoCell CC310 enabled — P-256 improves to 19.4 / 37.2, and Ed25519 becomes **unavailable** (`PSA_ERROR_NOT_SUPPORTED`) |
| `primitive-bench-*.conf` | The build overlays that produced each, so the comparison is reproducible |
| `identity-binding.log` | DAC signed by PAI, DAC key signing the tunnel public key, and a tampered key rejected |
| `signing-verification.log` | Forgery and replay rejected on hardware; altered counter, flipped direction, tampered payload and substituted device key each rejected |
| `tunnel-echo-roundtrip.log` | The transport alone: opaque bytes round-tripping through a commercial hub's border router, with the CASE handshake in view |
| `fabric-before.txt`, `fabric-after.txt` | Device-side fabric table either side of an ecosystem removal performed while the device was genuinely unreachable |

## Test vectors

Public material only. No private key appears in this repository.

| File | What it is |
|---|---|
| `dac.der` | The device's Matter Device Attestation Certificate |
| `pai.der` | The Product Attestation Intermediate that signed it |
| `tunnel_pub.bin` | The device's tunnel public key, 32 bytes |
| `dac_sig.bin` | The DAC key's signature over the tunnel public key |
| `identity-bundle.bin` | The complete bundle as fetched over the wire, 1060 bytes — about 90% X.509 |
| `resp_sig.bin`, `resp_meta.bin` | A captured signed response and its binding fields |

⚠️ These are **test** credentials from the public Matter development PKI, under the
test vendor ID `0xFFF1`. They attest to nothing and must never be treated as
production attestation. A shipping device carries a production DAC; the binding is
structurally identical, the trust root is not.
