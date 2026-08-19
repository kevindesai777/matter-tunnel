/*
 * PSA-backed signing for the VendorTunnel cluster. See tunnel_crypto.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_crypto.h"

#include <lib/support/logging/CHIPLogging.h>

namespace tunnel {
namespace {

struct AlgTraits {
	psa_key_type_t key_type;
	size_t key_bits;
	psa_algorithm_t alg;
};

AlgTraits Traits(Alg alg)
{
	switch (alg) {
	case Alg::Ed25519:
		/* PSA_ALG_PURE_EDDSA signs the message directly; there is no
		 * separate pre-hash step, which is part of Ed25519's appeal. */
		return { PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS), 255,
			 PSA_ALG_PURE_EDDSA };
	case Alg::EcdsaP256:
	default:
		/* ECDSA here is SHA-256-based, matching Matter's own attestation
		 * usage so the two share the accelerated code path. */
		return { PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1), 256,
			 PSA_ALG_ECDSA(PSA_ALG_SHA_256) };
	}
}

} // namespace

psa_status_t Signer::Init(Alg alg)
{
	if (Ready()) {
		return PSA_SUCCESS;
	}

	psa_status_t st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		ChipLogError(Zcl, "tunnel: psa_crypto_init failed: %d", static_cast<int>(st));
		return st;
	}

	const AlgTraits t = Traits(alg);

	/* The tunnel key must survive reboot: a client that has verified and
	 * stored this device's public key would otherwise find it invalid after
	 * every restart, and the DAC-bound identity would have to be re-fetched
	 * each time. So try a persistent PSA key first, keyed by a fixed id, and
	 * reuse it if it already exists. */
	const psa_key_id_t persistentId = kPersistentKeyId + static_cast<psa_key_id_t>(alg);

	/* psa_open_key() was removed in PSA Crypto 1.0; a persistent key is
	 * addressed directly by its id. Probe for an existing one by asking for
	 * its attributes. */
	{
		psa_key_attributes_t probe = PSA_KEY_ATTRIBUTES_INIT;
		if (psa_get_key_attributes(persistentId, &probe) == PSA_SUCCESS) {
			psa_reset_key_attributes(&probe);
			mKeyId = persistentId;
			mAlg = alg;
			ChipLogProgress(Zcl, "tunnel: reusing persistent key id=0x%x",
					static_cast<unsigned>(persistentId));
			return PSA_SUCCESS;
		}
		psa_reset_key_attributes(&probe);
	}

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE | PSA_KEY_USAGE_VERIFY_MESSAGE |
					       PSA_KEY_USAGE_EXPORT);
	psa_set_key_algorithm(&attr, t.alg);
	psa_set_key_type(&attr, t.key_type);
	psa_set_key_bits(&attr, t.key_bits);
	psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_PERSISTENT);
	psa_set_key_id(&attr, persistentId);

	st = psa_generate_key(&attr, &mKeyId);

	if (st != PSA_SUCCESS) {
		/* Persistent storage may be unavailable (no settings backend, or
		 * a measurement build). Fall back to a volatile key so the device
		 * still functions, but say so - silently losing persistence would
		 * be a subtle and damaging bug. */
		ChipLogError(Zcl, "tunnel: persistent key unavailable (%d); using volatile key - "
				  "clients must re-fetch identity after reboot",
			     static_cast<int>(st));
		psa_reset_key_attributes(&attr);

		psa_key_attributes_t vol = PSA_KEY_ATTRIBUTES_INIT;
		psa_set_key_usage_flags(&vol, PSA_KEY_USAGE_SIGN_MESSAGE |
						      PSA_KEY_USAGE_VERIFY_MESSAGE |
						      PSA_KEY_USAGE_EXPORT);
		psa_set_key_algorithm(&vol, t.alg);
		psa_set_key_type(&vol, t.key_type);
		psa_set_key_bits(&vol, t.key_bits);

		st = psa_generate_key(&vol, &mKeyId);
		psa_reset_key_attributes(&vol);

		if (st != PSA_SUCCESS) {
			ChipLogError(Zcl, "tunnel: psa_generate_key failed: %d",
				     static_cast<int>(st));
			return st;
		}
		mAlg = alg;
		return PSA_SUCCESS;
	}

	psa_reset_key_attributes(&attr);
	ChipLogProgress(Zcl, "tunnel: generated persistent key id=0x%x",
			static_cast<unsigned>(persistentId));
	mAlg = alg;
	return PSA_SUCCESS;
}

psa_status_t Signer::ExportPublicKey(uint8_t *out, size_t out_size, size_t *out_len)
{
	if (!Ready()) {
		return PSA_ERROR_BAD_STATE;
	}
	return psa_export_public_key(mKeyId, out, out_size, out_len);
}

psa_status_t Signer::Sign(const uint8_t *msg, size_t msg_len, uint8_t *sig, size_t sig_size,
			  size_t *sig_len)
{
	if (!Ready()) {
		return PSA_ERROR_BAD_STATE;
	}
	return psa_sign_message(mKeyId, Traits(mAlg).alg, msg, msg_len, sig, sig_size, sig_len);
}

psa_status_t Signer::Verify(const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len)
{
	if (!Ready()) {
		return PSA_ERROR_BAD_STATE;
	}
	return psa_verify_message(mKeyId, Traits(mAlg).alg, msg, msg_len, sig, sig_len);
}

psa_status_t ImportPublicKey(Alg alg, const uint8_t *key, size_t key_len, psa_key_id_t *out)
{
	const AlgTraits t = Traits(alg);

	psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
	psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_VERIFY_MESSAGE);
	psa_set_key_algorithm(&attr, t.alg);
	psa_set_key_type(&attr, PSA_KEY_TYPE_PUBLIC_KEY_OF_KEY_PAIR(t.key_type));
	psa_set_key_bits(&attr, t.key_bits);

	psa_status_t st = psa_import_key(&attr, key, key_len, out);
	psa_reset_key_attributes(&attr);
	return st;
}

psa_status_t VerifyWith(psa_key_id_t key, Alg alg, const uint8_t *msg, size_t msg_len,
			const uint8_t *sig, size_t sig_len)
{
	return psa_verify_message(key, Traits(alg).alg, msg, msg_len, sig, sig_len);
}

} // namespace tunnel
