# Security

This is research code accompanying a measurement paper. It has not been audited,
and it is not a product. The list below is not a disclaimer added for form — each
entry is a gap that was found deliberately and is stated in the paper. Read it
before building anything on this.

## What the mechanism does establish

Verified on hardware, each by tampering with the field in question and confirming
rejection:

- **Origin.** A payload was produced by a holder of the device's tunnel private
  key, or of the registered client's private key. Forged signatures are rejected.
- **Freshness.** A captured payload cannot be presented again. Replayed and stale
  counters are rejected; the reserved-ceiling guard means a reboot cannot move the
  floor backwards and reopen the window.
- **Binding.** The signature covers a domain tag, direction, counter, cluster ID,
  command ID, a length-prefixed device key and the payload. Altering any of them
  is rejected, so a signed message cannot be redirected to another command, another
  cluster or another device, and a response cannot be replayed as a request.
- **Key authenticity against the hub.** The device's tunnel public key is signed by
  its Matter Device Attestation Certificate key, and the DAC chains to a public CSA
  root. The hub holds no DAC private key, so it cannot substitute its own key
  undetected — which is what makes it safe to fetch the bundle through the
  hub-relayed tunnel itself, with no out-of-band channel.

## What it does not establish

### 1. The swap attack — specified, not implemented

DAC validation proves *a* genuine device of the expected VID/PID. It does not prove
*the user's* device. A hostile hub could substitute a different genuine unit of the
same model and every check above still passes.

The fix is a short fingerprint of the tunnel public key, printed beside the setup QR
code and checked by the client at setup — the same trust root as the commissioning
passcode, which the user already transcribes from the device. **This is specified
and not implemented here.** Do not read the DAC binding as a defence against it.

### 2. One counter space means one client

The replay guard keeps a single monotonic counter, so two clients would reject each
other's commands. Production needs a counter per client keyed by the client's public
key. No wire-format change is required for that; it was left out because the
measurement needed one client.

### 3. Client registration trusts physical presence

A client public key is registered by a button press on the device. That is the same
assumption Matter's own commissioning makes, and it fails in the same circumstances:
an attacker with physical access to the device can register their own key.

### 4. Test attestation, not production

Builds here use the Matter development PKI (VID `0xFFF1`) and test attestation
credentials. A shipping device carries a production DAC. The identity binding is
structurally identical; the trust root is not. Certificates under
`evidence/test-vectors/` are public test material and confer nothing.

### 5. The tunnel is confidential to the hub, not to the network

Payloads are authenticated end to end, not encrypted. A hub cannot forge or alter
them; it *can* read them. Matter's own CASE session encrypts the link, so the hub is
the observer of interest — but if payload confidentiality against the hub is
required, the envelope needs an AEAD layer, which this does not have.

## Reporting a vulnerability

Open an issue, or email kevindesai777@gmail.com. There is no coordinated-disclosure
process and no security-fix SLA; treat this as research code.
