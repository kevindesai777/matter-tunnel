/*
 * DAC-bound tunnel identity. See tunnel_identity.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_identity.h"

#include <credentials/DeviceAttestationCredsProvider.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>

using namespace chip;

namespace tunnel {
namespace {

/* DER certificates dominate the bundle; sized generously and checked at
 * runtime rather than assumed. */
constexpr size_t kCertBufSize = 640;
constexpr size_t kDacSigSize = 64; /* ECDSA P-256 r||s - the DAC key is always P-256 per spec */

class Writer {
public:
	Writer(uint8_t *buf, size_t cap) : mBuf(buf), mCap(cap) {}

	bool U8(uint8_t v)
	{
		if (mLen + 1 > mCap) {
			return false;
		}
		mBuf[mLen++] = v;
		return true;
	}

	bool Field(const ByteSpan &s)
	{
		if (s.size() > UINT16_MAX || mLen + 2 + s.size() > mCap) {
			return false;
		}
		mBuf[mLen++] = static_cast<uint8_t>(s.size() >> 8);
		mBuf[mLen++] = static_cast<uint8_t>(s.size() & 0xFF);
		memcpy(mBuf + mLen, s.data(), s.size());
		mLen += s.size();
		return true;
	}

	size_t Length() const { return mLen; }

private:
	uint8_t *mBuf;
	size_t mCap;
	size_t mLen = 0;
};

} // namespace

CHIP_ERROR BuildIdentityBundle(Signer &signer, MutableByteSpan &out)
{
	VerifyOrReturnError(signer.Ready(), CHIP_ERROR_INCORRECT_STATE);

	auto *dacProvider = Credentials::GetDeviceAttestationCredentialsProvider();
	VerifyOrReturnError(dacProvider != nullptr, CHIP_ERROR_INCORRECT_STATE);

	/* 1. tunnel public key */
	uint8_t pubkey[kPublicKeyLenMax];
	size_t pubkeyLen = 0;
	psa_status_t st = signer.ExportPublicKey(pubkey, sizeof(pubkey), &pubkeyLen);
	VerifyOrReturnError(st == PSA_SUCCESS, CHIP_ERROR_INTERNAL);

	const uint8_t alg = (signer.Algorithm() == Alg::Ed25519) ? 0 : 1;

	/* 2. the preimage the DAC key signs: version || alg || pubkey.
	 *    Binding the version and algorithm prevents a downgrade in which an
	 *    attacker replays this signature to assert a weaker algorithm. */
	uint8_t preimage[2 + kPublicKeyLenMax];
	preimage[0] = kIdentityBundleVersion;
	preimage[1] = alg;
	memcpy(preimage + 2, pubkey, pubkeyLen);
	const size_t preimageLen = 2 + pubkeyLen;

	uint8_t dacSig[kDacSigSize];
	MutableByteSpan dacSigSpan(dacSig);
	ReturnErrorOnFailure(dacProvider->SignWithDeviceAttestationKey(
		ByteSpan(preimage, preimageLen), dacSigSpan));

	/* 3. the certificate chain */
	uint8_t dacBuf[kCertBufSize];
	MutableByteSpan dac(dacBuf);
	ReturnErrorOnFailure(dacProvider->GetDeviceAttestationCert(dac));

	uint8_t paiBuf[kCertBufSize];
	MutableByteSpan pai(paiBuf);
	ReturnErrorOnFailure(dacProvider->GetProductAttestationIntermediateCert(pai));

	/* 4. serialise */
	Writer w(out.data(), out.size());
	VerifyOrReturnError(w.U8(kIdentityBundleVersion), CHIP_ERROR_BUFFER_TOO_SMALL);
	VerifyOrReturnError(w.U8(alg), CHIP_ERROR_BUFFER_TOO_SMALL);
	VerifyOrReturnError(w.Field(ByteSpan(pubkey, pubkeyLen)), CHIP_ERROR_BUFFER_TOO_SMALL);
	VerifyOrReturnError(w.Field(dacSigSpan), CHIP_ERROR_BUFFER_TOO_SMALL);
	VerifyOrReturnError(w.Field(dac), CHIP_ERROR_BUFFER_TOO_SMALL);
	VerifyOrReturnError(w.Field(pai), CHIP_ERROR_BUFFER_TOO_SMALL);

	out.reduce_size(w.Length());
	return CHIP_NO_ERROR;
}

void ReportIdentityBundle()
{
	Signer signer;
	psa_status_t st = signer.Init(Alg::Ed25519);
	if (st != PSA_SUCCESS) {
		ChipLogError(Zcl, "tunnel-identity: signer init failed: %d", static_cast<int>(st));
		return;
	}

	static uint8_t bundle[1600];
	MutableByteSpan span(bundle);

	CHIP_ERROR err = BuildIdentityBundle(signer, span);
	if (err != CHIP_NO_ERROR) {
		ChipLogError(Zcl, "tunnel-identity: BuildIdentityBundle failed: %" CHIP_ERROR_FORMAT,
			     err.Format());
		return;
	}

	/* Re-read the field sizes straight out of the serialised bundle, so the
	 * report describes what was actually produced rather than what we think
	 * was produced. */
	const uint8_t *p = bundle;
	const uint8_t version = *p++;
	const uint8_t alg = *p++;
	auto fieldLen = [&p]() -> size_t {
		size_t n = (static_cast<size_t>(p[0]) << 8) | p[1];
		p += 2 + n;
		return n;
	};
	const size_t pubLen = fieldLen();
	const size_t sigLen = fieldLen();
	const size_t dacLen = fieldLen();
	const size_t paiLen = fieldLen();

	ChipLogProgress(Zcl, "tunnel-identity: v%u alg=%u pubkey=%u dac_sig=%u DAC=%u PAI=%u TOTAL=%u B",
			version, alg, static_cast<unsigned>(pubLen), static_cast<unsigned>(sigLen),
			static_cast<unsigned>(dacLen), static_cast<unsigned>(paiLen),
			static_cast<unsigned>(span.size()));

	ChipLogProgress(Zcl, "tunnel-identity: fits in one 512 B tunnel payload? %s",
			span.size() <= 512 ? "yes" : "NO - chunked transfer required");
}

} // namespace tunnel
