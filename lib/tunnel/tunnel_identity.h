/*
 * Device identity for the VendorTunnel: binding a per-device tunnel signing key
 * to Matter's existing device attestation.
 *
 * The problem
 * -----------
 * Application-layer signing is worthless if the hub can substitute its own
 * public key: the signature would then prove "someone the hub chose" rather
 * than "the device". So the client must learn the device's tunnel public key
 * over a path the hub cannot tamper with.
 *
 * The approach
 * ------------
 * Matter already solves "prove this is a genuine device from vendor X": the
 * Device Attestation Certificate (DAC) chains to a Product Attestation
 * Intermediate (PAI) and then to a Product Attestation Authority (PAA) root
 * published by the CSA. The hub holds no device's DAC private key, so it cannot
 * forge that chain.
 *
 * The device therefore mints its own tunnel keypair and certifies it with the
 * attestation key it already has:
 *
 *     bundle = { tunnel_pubkey,
 *                Sign_DAC(version || alg || tunnel_pubkey),
 *                DAC, PAI }
 *
 * A client validates DAC->PAI->PAA against public roots, verifies the signature
 * over the tunnel public key, and then trusts that key. No out-of-band channel,
 * no vendor backend, no administrator privileges - and the bundle can travel
 * through the hub-relayed tunnel itself, because tampering is detectable.
 *
 * Known limitation - swap attack
 * ------------------------------
 * This proves the key belongs to *a* genuine device of this VID/PID, not to
 * *the* device the user owns. A hostile hub could substitute a different
 * genuine unit of the same product. Closing that requires binding to something
 * the user physically possesses - a short fingerprint of the tunnel key printed
 * alongside the setup QR code, checked by the client at setup. That is the
 * documented production hardening; it is not implemented here, and the residual
 * gap is stated rather than hidden.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tunnel_crypto.h"

#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>

namespace tunnel {

constexpr uint8_t kIdentityBundleVersion = 1;

/*
 * Serialise the identity bundle into `out`.
 *
 * Layout (all lengths big-endian u16):
 *   u8   version
 *   u8   alg              0 = Ed25519, 1 = ECDSA P-256
 *   u16  pubkey_len   + pubkey
 *   u16  dac_sig_len  + dac_sig     signature by the DAC key over
 *                                   (version || alg || pubkey)
 *   u16  dac_len      + DAC  (DER)
 *   u16  pai_len      + PAI  (DER)
 *
 * On success `out` is reduced to the bytes written.
 */
CHIP_ERROR BuildIdentityBundle(Signer &signer, chip::MutableByteSpan &out);

/* Log the bundle's composition and per-field sizes. Used to determine whether
 * the bundle fits a single tunnel payload or requires chunked transfer. */
void ReportIdentityBundle();

} // namespace tunnel
