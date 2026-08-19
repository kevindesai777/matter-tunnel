/*
 * Transport-agnostic core of the VendorTunnel payload protocol.
 *
 * The envelope protocol (see tunnel_server.cpp for the wire format) is
 * deliberately independent of how the bytes arrive. Matter's vendor cluster is
 * one carrier; a direct BLE link is another. Both call ProcessRequest(), so the
 * signing, replay and binding path is *the same code* in both conditions — which
 * is what makes the measured difference attributable to transport alone rather
 * than to two similar-looking implementations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

namespace tunnel {

/* Why a request produced no response payload. Transports map these onto
 * whatever their own error channel is: Matter to an InteractionModel status,
 * BLE to silence (the client times out). */
enum class Reject : uint8_t {
	None = 0,
	InvalidCommand,
	ConstraintError,
	Failure,
};

/* Largest response the protocol can emit: an identity chunk (5 + 400). */
constexpr size_t kMaxResponse = 512;

/*
 * Process one request envelope and write the response envelope to `out`.
 *
 * Returns the number of bytes written, or 0 when there is no payload response,
 * in which case *reject says why.
 */
size_t ProcessRequest(const uint8_t *in, size_t inLen, uint8_t *out, size_t outCap,
		      Reject *reject);

} // namespace tunnel
