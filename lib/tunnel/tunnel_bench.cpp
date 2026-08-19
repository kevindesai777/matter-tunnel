/*
 * On-device sign/verify benchmark, measured with the Cortex-M DWT cycle
 * counter.
 *
 * This is the paper's PRIMARY cost evidence. End-to-end network latency cannot
 * resolve a signing operation: the measured Thread noise floor is ~252 ms with
 * sigma ~116 ms even on a non-sleepy device, which is three orders of magnitude
 * above the signal. Measuring on-device sidesteps the network entirely.
 *
 * Why DWT and not k_cycle_get_32(): on nRF52840 Zephyr's kernel clock is backed
 * by the 32.768 kHz RTC, giving ~30 us granularity - far too coarse when the
 * operation under test may itself be under a millisecond. DWT counts CPU cycles
 * at 64 MHz.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_crypto.h"

#include <cmsis_core.h>
#include <lib/support/logging/CHIPLogging.h>
#include <zephyr/kernel.h>

namespace tunnel {
namespace {

constexpr uint32_t kCpuHz = 64000000u; /* nRF52840 */
constexpr int kIterations = 20;
constexpr size_t kMsgLen = 64; /* representative signed-envelope preimage */

bool DwtInit()
{
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0;
	DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
	/* Some cores gate CYCCNT; confirm it actually advances. */
	const uint32_t a = DWT->CYCCNT;
	for (volatile int i = 0; i < 10; i++) {
	}
	return DWT->CYCCNT != a;
}

inline uint32_t Cycles()
{
	return DWT->CYCCNT;
}

struct Stats {
	uint32_t min, max;
	uint64_t total;
	int n;
	uint32_t avg() const { return n ? static_cast<uint32_t>(total / n) : 0; }
	uint32_t us(uint32_t cyc) const { return static_cast<uint32_t>((uint64_t)cyc * 1000000u / kCpuHz); }
};

void Accumulate(Stats &s, uint32_t c)
{
	if (s.n == 0 || c < s.min) {
		s.min = c;
	}
	if (c > s.max) {
		s.max = c;
	}
	s.total += c;
	s.n++;
}

void Report(const char *label, const Stats &s)
{
	ChipLogProgress(Zcl, "tunnel-bench: %-18s n=%d  min=%u cyc (%u us)  avg=%u cyc (%u us)  max=%u cyc (%u us)",
			label, s.n, s.min, s.us(s.min), s.avg(), s.us(s.avg()), s.max, s.us(s.max));
}

void BenchmarkOne(Alg alg, const char *name)
{
	Signer signer;
	uint8_t msg[kMsgLen];
	uint8_t sig[kSignatureLen];
	uint8_t pub[kPublicKeyLenMax];
	size_t sig_len = 0, pub_len = 0;

	for (size_t i = 0; i < sizeof(msg); i++) {
		msg[i] = static_cast<uint8_t>(i);
	}

	uint32_t t0 = Cycles();
	psa_status_t st = signer.Init(alg);
	uint32_t keygen = Cycles() - t0;

	if (st != PSA_SUCCESS) {
		ChipLogError(Zcl, "tunnel-bench: %s UNAVAILABLE (keygen status %d)", name,
			     static_cast<int>(st));
		return;
	}

	st = signer.ExportPublicKey(pub, sizeof(pub), &pub_len);
	if (st != PSA_SUCCESS) {
		ChipLogError(Zcl, "tunnel-bench: %s export failed: %d", name, static_cast<int>(st));
		return;
	}

	ChipLogProgress(Zcl, "tunnel-bench: === %s === pubkey=%u B  keygen=%u cyc (%u us)", name,
			static_cast<unsigned>(pub_len), keygen,
			static_cast<unsigned>((uint64_t)keygen * 1000000u / kCpuHz));

	Stats sign{}, verify{};

	for (int i = 0; i < kIterations; i++) {
		msg[0] = static_cast<uint8_t>(i); /* defeat any caching */

		t0 = Cycles();
		st = signer.Sign(msg, sizeof(msg), sig, sizeof(sig), &sig_len);
		uint32_t c = Cycles() - t0;
		if (st != PSA_SUCCESS) {
			ChipLogError(Zcl, "tunnel-bench: %s sign failed: %d", name,
				     static_cast<int>(st));
			return;
		}
		Accumulate(sign, c);

		t0 = Cycles();
		st = signer.Verify(msg, sizeof(msg), sig, sig_len);
		c = Cycles() - t0;
		if (st != PSA_SUCCESS) {
			ChipLogError(Zcl, "tunnel-bench: %s VERIFY FAILED: %d", name,
				     static_cast<int>(st));
			return;
		}
		Accumulate(verify, c);
	}

	ChipLogProgress(Zcl, "tunnel-bench: %s signature=%u B", name,
			static_cast<unsigned>(sig_len));
	Report("sign", sign);
	Report("verify", verify);

	/* Negative control: a tampered message must fail to verify. A benchmark
	 * that only ever exercises the success path can silently measure a
	 * no-op. */
	msg[0] ^= 0xFF;
	st = signer.Verify(msg, sizeof(msg), sig, sig_len);
	ChipLogProgress(Zcl, "tunnel-bench: %s tamper-check %s", name,
			(st == PSA_SUCCESS) ? "*** FAILED - BAD SIGNATURE ACCEPTED ***"
					    : "ok (rejected)");
}

} // namespace

void RunCryptoBenchmark()
{
	if (!DwtInit()) {
		ChipLogError(Zcl, "tunnel-bench: DWT cycle counter unavailable; skipping");
		return;
	}

	ChipLogProgress(Zcl, "tunnel-bench: starting (cpu=%u Hz, iterations=%d, msg=%u B)", kCpuHz,
			kIterations, static_cast<unsigned>(kMsgLen));

	BenchmarkOne(Alg::EcdsaP256, "ECDSA-P256");
	BenchmarkOne(Alg::Ed25519, "Ed25519");

	ChipLogProgress(Zcl, "tunnel-bench: done");
}

} // namespace tunnel
