/*
 * VendorTunnel cluster — server-side command handler.
 *
 * The cluster carries a single opaque request/response command. All structure
 * lives in this payload protocol, deliberately: Matter models none of it, so
 * the hub cannot interpret or forge it, and the cluster surface stays minimal
 * (one command, two octet strings) which keeps flash cost down.
 *
 * Envelope
 * --------
 *   request : u8 type, body
 *   response: u8 type, body
 *
 *   0x00 IdentityRequest    u16 offset
 *   0x01 IdentityResponse   u16 offset, u16 total, bytes chunk
 *   0x02 SignedRequest      u32 counter, u16+payload, u16+client_sig
 *   0x03 SignedResponse     u32 counter, u16+payload, u16+device_sig
 *   0x04 ReplayReject       u32 device_floor
 *   0x05 RegisterClientKey  u16+client_pubkey
 *   0x06 RegisterAck        u16+stored_pubkey, u16+device_sig
 *   0x07 AuthReject         (no key registered, bad signature, or window shut)
 *   0xFF Echo               bytes    (unsigned control condition for latency
 *                                     measurement; carries no authority)
 *
 * The identity bundle (tunnel public key certified by the device's Matter DAC —
 * see tunnel_identity.h) is 1060 bytes on this device, of which ~90% is X.509.
 * That exceeds one payload, so it is fetched in chunks. It is static per
 * device, so this happens once at client setup, never per command.
 *
 * Contains no device-type semantics: this compiles unchanged into a lock, a
 * thermostat, or a switch.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_core.h"

#if defined(CONFIG_TUNNEL_SECURITY)
#include "tunnel_crypto.h"
#include "tunnel_envelope.h"
#include "tunnel_identity.h"
#include "tunnel_replay.h"
#endif

#if defined(CONFIG_TUNNEL_CLIENT_AUTH)
#include "tunnel_client_key.h"
#endif

#include <app-common/zap-generated/callback.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/CommandHandler.h>
#include <app/ConcreteCommandPath.h>
#include <lib/support/logging/CHIPLogging.h>

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::app::Clusters;

namespace {

constexpr uint8_t kTypeEcho = 0xFF;

#if defined(CONFIG_TUNNEL_SECURITY)
constexpr uint8_t kTypeIdentityRequest = 0x00;
constexpr uint8_t kTypeIdentityResponse = 0x01;
constexpr uint8_t kTypeSignedRequest = 0x02;
constexpr uint8_t kTypeSignedResponse = 0x03;
constexpr uint8_t kTypeReplayReject = 0x04;
#endif

#if defined(CONFIG_TUNNEL_CLIENT_AUTH)
constexpr uint8_t kTypeRegisterClientKey = 0x05;
constexpr uint8_t kTypeRegisterAck = 0x06;
constexpr uint8_t kTypeAuthReject = 0x07;
#endif

#if defined(CONFIG_TUNNEL_SECURITY)

/* Response framing is 5 bytes (type, offset, total); keep the chunk well inside
 * the 512-byte payload ceiling declared in the cluster XML. */
constexpr size_t kMaxChunk = 400;

/* The identity bundle is expensive to build - it involves a DAC signature - and
 * is immutable for the life of the device's key. Build once, serve many. */
tunnel::Signer gSigner;
uint8_t gBundle[1200];
size_t gBundleLen = 0;

tunnel::ReplayGuard gReplay;
bool gReplayReady = false;

/* The device's own tunnel public key, cached for use in the binding preimage. */
uint8_t gDevicePub[tunnel::kPublicKeyLenMax];
size_t gDevicePubLen = 0;

bool EnsureBundle()
{
	if (gBundleLen != 0) {
		return true;
	}

	if (gSigner.Init(tunnel::Alg::Ed25519) != PSA_SUCCESS) {
		return false;
	}

	MutableByteSpan span(gBundle);
	if (tunnel::BuildIdentityBundle(gSigner, span) != CHIP_NO_ERROR) {
		return false;
	}

	gBundleLen = span.size();

	if (gSigner.ExportPublicKey(gDevicePub, sizeof(gDevicePub), &gDevicePubLen) != PSA_SUCCESS) {
		gBundleLen = 0;
		return false;
	}

	ChipLogProgress(Zcl, "VendorTunnel: identity bundle ready (%u B)",
			static_cast<unsigned>(gBundleLen));
	return true;
}

bool EnsureReplay()
{
	if (gReplayReady) {
		return true;
	}
	if (gReplay.Init() != CHIP_NO_ERROR) {
		return false;
	}
	gReplayReady = true;
#if defined(CONFIG_TUNNEL_CLIENT_AUTH)
	/* The key store shares this lazy-init point rather than having its own:
	 * both are needed by the first signed request and by nothing earlier. */
	tunnel::GetClientKeyStore().Init();
#endif
	return true;
}

uint32_t ReadU32(const uint8_t *p)
{
	return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
	       (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void PutU32(uint8_t *p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v >> 24);
	p[1] = static_cast<uint8_t>(v >> 16);
	p[2] = static_cast<uint8_t>(v >> 8);
	p[3] = static_cast<uint8_t>(v);
}

#endif /* CONFIG_TUNNEL_SECURITY */

} // namespace

size_t tunnel::ProcessRequest(const uint8_t *inData, size_t inLen, uint8_t *out, size_t outCap,
			      tunnel::Reject *reject)
{
	*reject = tunnel::Reject::None;

	const ByteSpan in(inData, inLen);

	if (in.size() < 1) {
		ChipLogError(Zcl, "VendorTunnel: empty payload");
		*reject = tunnel::Reject::InvalidCommand;
		return 0;
	}

	const uint8_t type = in.data()[0];

	switch (type) {
	case kTypeEcho: {
		/* Unsigned control condition. Echoes the body verbatim; the
		 * payload is never interpreted. */
		ChipLogProgress(Zcl, "VendorTunnel: echo len=%u",
				static_cast<unsigned>(in.size() - 1));
		if (in.size() > outCap) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}
		memcpy(out, in.data(), in.size());
		return in.size();
	}

#if defined(CONFIG_TUNNEL_SECURITY)
	case kTypeIdentityRequest: {
		if (in.size() < 3) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}
		const size_t offset = (static_cast<size_t>(in.data()[1]) << 8) | in.data()[2];

		if (!EnsureBundle()) {
			ChipLogError(Zcl, "VendorTunnel: identity unavailable");
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		if (offset >= gBundleLen) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}

		const size_t remaining = gBundleLen - offset;
		const size_t chunk = remaining < kMaxChunk ? remaining : kMaxChunk;

		if (5 + chunk > outCap) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}
		out[0] = kTypeIdentityResponse;
		out[1] = static_cast<uint8_t>(offset >> 8);
		out[2] = static_cast<uint8_t>(offset & 0xFF);
		out[3] = static_cast<uint8_t>(gBundleLen >> 8);
		out[4] = static_cast<uint8_t>(gBundleLen & 0xFF);
		memcpy(out + 5, gBundle + offset, chunk);

		ChipLogProgress(Zcl, "VendorTunnel: identity chunk offset=%u len=%u of %u",
				static_cast<unsigned>(offset), static_cast<unsigned>(chunk),
				static_cast<unsigned>(gBundleLen));

		return 5 + chunk;
	}

	case kTypeSignedRequest: {
		/* Layout: u8 type, u32 counter, u16 len + payload,
		 *         u16 len + client_sig
		 *
		 * Both directions are authenticated: the request is verified
		 * against the registered client key (tunnel_client_key.h) and the
		 * response is signed with the device's DAC-bound tunnel key
		 * (tunnel_identity.h). Order matters - authenticity is checked
		 * before freshness, so an unauthenticated peer cannot drive the
		 * replay counter, or the flash writes it triggers, forward. */
		if (in.size() < 7) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}

		const uint32_t counter = ReadU32(in.data() + 1);
		const size_t payloadLen = (static_cast<size_t>(in.data()[5]) << 8) | in.data()[6];

		if (in.size() < 7 + payloadLen + 2) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}
		const ByteSpan payload(in.data() + 7, payloadLen);

		const size_t reqSigLen = (static_cast<size_t>(in.data()[7 + payloadLen]) << 8) |
					 in.data()[8 + payloadLen];
		if (in.size() < 9 + payloadLen + reqSigLen) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}
		const uint8_t *reqSig = in.data() + 9 + payloadLen;

		if (!EnsureBundle() || !EnsureReplay()) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		/* Authenticity before freshness: an unauthenticated peer must not
		 * be able to drive the replay counter - or the flash writes it
		 * triggers - forward. */
#if defined(CONFIG_TUNNEL_CLIENT_AUTH)
		auto &store = tunnel::GetClientKeyStore();
		if (!store.HasKey()) {
			ChipLogError(Zcl, "VendorTunnel: no client key registered; command refused");
			out[0] = kTypeAuthReject;
			return 1;
		}

		{
			static uint8_t reqPre[700];
			MutableByteSpan pre(reqPre);
			if (tunnel::BuildPreimage(tunnel::kDirRequest, counter,
						  ByteSpan(gDevicePub, gDevicePubLen), payload,
						  pre) != CHIP_NO_ERROR) {
				*reject = tunnel::Reject::ConstraintError;
				return 0;
			}
			if (store.Verify(pre.data(), pre.size(), reqSig, reqSigLen) != PSA_SUCCESS) {
				ChipLogError(Zcl, "VendorTunnel: BAD CLIENT SIGNATURE counter=%u",
					     static_cast<unsigned>(counter));
				out[0] = kTypeAuthReject;
				return 1;
			}
		}
#else
		/* Requests are accepted unverified in this configuration. The
		 * signature field is still parsed so the wire format is
		 * unchanged, but nothing is checked against it. */
		(void)reqSig;
		(void)reqSigLen;
#endif /* CONFIG_TUNNEL_CLIENT_AUTH */

		/* Freshness first: reject a stale counter and tell the client the
		 * current floor so it can resynchronise without a separate
		 * exchange. */
		if (!gReplay.IsFresh(counter)) {
			ChipLogProgress(Zcl,
					"VendorTunnel: REPLAY REJECTED counter=%u floor=%u",
					static_cast<unsigned>(counter),
					static_cast<unsigned>(gReplay.Floor()));
			out[0] = kTypeReplayReject;
			PutU32(out + 1, gReplay.Floor());
			return 5;
		}

		if (gReplay.Accept(counter) != CHIP_NO_ERROR) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		/* Sign the response over the bound preimage. */
		static uint8_t preimageBuf[700];
		MutableByteSpan preimage(preimageBuf);
		if (tunnel::BuildPreimage(tunnel::kDirResponse, counter,
					  ByteSpan(gDevicePub, gDevicePubLen), payload,
					  preimage) != CHIP_NO_ERROR) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}

		uint8_t sig[tunnel::kSignatureLen];
		size_t sigLen = 0;
		if (gSigner.Sign(preimage.data(), preimage.size(), sig, sizeof(sig), &sigLen) !=
		    PSA_SUCCESS) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		if (payloadLen > 256 || 9 + payloadLen + tunnel::kSignatureLen > outCap) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}
		size_t n = 0;
		out[n++] = kTypeSignedResponse;
		PutU32(out + n, counter);
		n += 4;
		out[n++] = static_cast<uint8_t>(payloadLen >> 8);
		out[n++] = static_cast<uint8_t>(payloadLen & 0xFF);
		memcpy(out + n, payload.data(), payloadLen);
		n += payloadLen;
		out[n++] = static_cast<uint8_t>(sigLen >> 8);
		out[n++] = static_cast<uint8_t>(sigLen & 0xFF);
		memcpy(out + n, sig, sigLen);
		n += sigLen;

		ChipLogProgress(Zcl, "VendorTunnel: signed response counter=%u payload=%u sig=%u",
				static_cast<unsigned>(counter), static_cast<unsigned>(payloadLen),
				static_cast<unsigned>(sigLen));

		return n;
	}

#if defined(CONFIG_TUNNEL_CLIENT_AUTH)
	case kTypeRegisterClientKey: {
		/* Layout: u8 type, u16 len + client_pubkey
		 *
		 * Accepted only while the physical-presence window is open. The
		 * device echoes the key it actually stored, signed with its own
		 * tunnel key, so a client can detect a hub that won a race - a
		 * silent failure would otherwise leave the client believing it
		 * had registered successfully. */
		if (in.size() < 3) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}
		const size_t keyLen = (static_cast<size_t>(in.data()[1]) << 8) | in.data()[2];
		if (in.size() < 3 + keyLen) {
			*reject = tunnel::Reject::InvalidCommand;
			return 0;
		}

		if (!EnsureBundle()) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		auto &store = tunnel::GetClientKeyStore();
		const CHIP_ERROR err = store.Register(ByteSpan(in.data() + 3, keyLen));

		if (err != CHIP_NO_ERROR) {
			ChipLogError(Zcl, "VendorTunnel: registration refused (window %s)",
				     store.WindowOpen() ? "open" : "CLOSED");
			out[0] = kTypeAuthReject;
			return 1;
		}

		/* Sign the stored key so the client can verify what was recorded. */
		const ByteSpan stored = store.Key();
		static uint8_t pre[256];
		MutableByteSpan preSpan(pre);
		if (tunnel::BuildPreimage(tunnel::kDirResponse, 0,
					  ByteSpan(gDevicePub, gDevicePubLen), stored,
					  preSpan) != CHIP_NO_ERROR) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		uint8_t sig[tunnel::kSignatureLen];
		size_t sigLen = 0;
		if (gSigner.Sign(preSpan.data(), preSpan.size(), sig, sizeof(sig), &sigLen) !=
		    PSA_SUCCESS) {
			*reject = tunnel::Reject::Failure;
			return 0;
		}

		if (5 + stored.size() + tunnel::kSignatureLen > outCap) {
			*reject = tunnel::Reject::ConstraintError;
			return 0;
		}
		size_t n = 0;
		out[n++] = kTypeRegisterAck;
		out[n++] = static_cast<uint8_t>(stored.size() >> 8);
		out[n++] = static_cast<uint8_t>(stored.size() & 0xFF);
		memcpy(out + n, stored.data(), stored.size());
		n += stored.size();
		out[n++] = static_cast<uint8_t>(sigLen >> 8);
		out[n++] = static_cast<uint8_t>(sigLen & 0xFF);
		memcpy(out + n, sig, sigLen);
		n += sigLen;

		ChipLogProgress(Zcl, "VendorTunnel: registration ack (key=%u B, sig=%u B)",
				static_cast<unsigned>(stored.size()), static_cast<unsigned>(sigLen));
		return n;
	}
#endif /* CONFIG_TUNNEL_CLIENT_AUTH */
#endif /* CONFIG_TUNNEL_SECURITY */

	default:
		ChipLogError(Zcl, "VendorTunnel: unknown envelope type 0x%02x", type);
		*reject = tunnel::Reject::InvalidCommand;
		return 0;
	}
}

/*
 * Matter transport adapter.
 *
 * Thin by design: it decodes the cluster command, hands the opaque bytes to the
 * transport-agnostic core, and maps a rejection onto an InteractionModel status.
 * The BLE adapter (tunnel_ble.cpp) is the same shape over a different carrier.
 */
bool emberAfVendorTunnelClusterTunnelRequestCallback(
	CommandHandler *commandObj, const ConcreteCommandPath &commandPath,
	const VendorTunnel::Commands::TunnelRequest::DecodableType &commandData)
{
	if (commandObj == nullptr) {
		return false;
	}

	static uint8_t out[tunnel::kMaxResponse];
	tunnel::Reject reject = tunnel::Reject::None;

	const size_t n = tunnel::ProcessRequest(commandData.payload.data(),
						commandData.payload.size(), out, sizeof(out),
						&reject);

	if (n == 0) {
		using Status = Protocols::InteractionModel::Status;
		Status status = Status::Failure;
		switch (reject) {
		case tunnel::Reject::InvalidCommand:
			status = Status::InvalidCommand;
			break;
		case tunnel::Reject::ConstraintError:
			status = Status::ConstraintError;
			break;
		default:
			status = Status::Failure;
			break;
		}
		commandObj->AddStatus(commandPath, status);
		return true;
	}

	VendorTunnel::Commands::TunnelResponse::Type response;
	response.payload = ByteSpan(out, n);
	commandObj->AddResponse(commandPath, response);
	return true;
}

namespace tunnel {
#if defined(CONFIG_TUNNEL_CRYPTO_BENCHMARK)
void RunCryptoBenchmark();
void ReportIdentityBundle();
#endif
#ifdef CONFIG_BT_NUS
void BleTransportScheduleInit();
#endif
} // namespace tunnel

void MatterVendorTunnelPluginServerInitCallback()
{
	ChipLogProgress(Zcl, "VendorTunnel: server initialised");

#ifdef CONFIG_BT_NUS
	/* Second transport for the measurement floor (condition 0). Same core,
	 * different carrier — see tunnel_ble.cpp. */
	tunnel::BleTransportScheduleInit();
#endif

#ifdef CONFIG_TUNNEL_CRYPTO_BENCHMARK
	tunnel::RunCryptoBenchmark();
	tunnel::ReportIdentityBundle();
#endif
}
