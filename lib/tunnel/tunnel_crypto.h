/*
 * Application-layer signing for the VendorTunnel cluster.
 *
 * Built on the PSA Crypto API so the algorithm is a build-time choice rather
 * than a structural one. That matters on nRF52840, where the intuitive pick is
 * not obviously the right one:
 *
 *   ECDSA P-256  - accelerated by CryptoCell CC310, and already compiled into
 *                  the image because Matter's attestation stack uses it, so it
 *                  costs no additional flash.
 *   Ed25519      - provided by nrf_oberon as optimised *software*; CC310 does
 *                  not accelerate it, and enabling it adds flash.
 *
 * Which is actually cheaper here is an empirical question. Both are therefore
 * selectable and measurable; see tunnel_bench.cpp.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <psa/crypto.h>
#include <stddef.h>
#include <stdint.h>

namespace tunnel {

/* Both algorithms produce a 64-byte signature:
 *   P-256   r || s        (32 + 32)
 *   Ed25519 R || S        (32 + 32)
 */
constexpr size_t kSignatureLen = 64;

/* Public key export sizes.
 *   P-256   uncompressed SEC1 point: 0x04 || X || Y
 *   Ed25519 raw 32-byte point
 */
constexpr size_t kPublicKeyLenMax = 65;

enum class Alg {
	EcdsaP256,
	Ed25519,
};

/* Base PSA key id for the device's persistent tunnel key. The algorithm index
 * is added, so switching algorithms does not collide. Chosen in the
 * application range; adjust if it clashes with another component. */
constexpr psa_key_id_t kPersistentKeyId = 0x00005400;

/*
 * Device-side signer. Owns one long-lived keypair.
 *
 * The key is generated in PSA and referenced by handle; the private key is
 * never exported. On a part with KMU/CRACEN it would additionally be hardware
 * resident - on nRF52840 it lives in PSA's software key store, which is a
 * limitation worth stating rather than glossing.
 */
class Signer {
public:
	/* Generate a fresh keypair. Idempotent within a boot. */
	psa_status_t Init(Alg alg);

	/* Export the public key. This is the value a client must obtain
	 * out-of-band; see the key-distribution discussion in the README. */
	psa_status_t ExportPublicKey(uint8_t *out, size_t out_size, size_t *out_len);

	/* Sign `msg`. `sig` must be at least kSignatureLen bytes. */
	psa_status_t Sign(const uint8_t *msg, size_t msg_len, uint8_t *sig, size_t sig_size,
			  size_t *sig_len);

	/* Verify a signature made by *this* device's key. Verifying a peer's
	 * signature uses VerifyWith() against an imported public key. */
	psa_status_t Verify(const uint8_t *msg, size_t msg_len, const uint8_t *sig,
			    size_t sig_len);

	Alg Algorithm() const { return mAlg; }
	bool Ready() const { return mKeyId != PSA_KEY_ID_NULL; }

private:
	psa_key_id_t mKeyId = PSA_KEY_ID_NULL;
	Alg mAlg = Alg::EcdsaP256;
};

/* Import a peer public key for verification, returning a key handle. */
psa_status_t ImportPublicKey(Alg alg, const uint8_t *key, size_t key_len, psa_key_id_t *out);

/* Verify `msg`/`sig` against a previously imported peer key. */
psa_status_t VerifyWith(psa_key_id_t key, Alg alg, const uint8_t *msg, size_t msg_len,
			const uint8_t *sig, size_t sig_len);

} // namespace tunnel
