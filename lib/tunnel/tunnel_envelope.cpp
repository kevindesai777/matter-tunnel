/*
 * Signature preimage construction. See tunnel_envelope.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_envelope.h"

#include <lib/support/CodeUtils.h>
#include <string.h>

using namespace chip;

namespace tunnel {
namespace {

constexpr char kDomainTag[] = "MTUNv1";
constexpr size_t kDomainTagLen = sizeof(kDomainTag) - 1; /* no NUL */

void PutU32(uint8_t *p, uint32_t v)
{
	p[0] = static_cast<uint8_t>(v >> 24);
	p[1] = static_cast<uint8_t>(v >> 16);
	p[2] = static_cast<uint8_t>(v >> 8);
	p[3] = static_cast<uint8_t>(v);
}

} // namespace

CHIP_ERROR BuildPreimage(uint8_t direction, uint32_t counter, const ByteSpan &deviceKey,
			 const ByteSpan &payload, MutableByteSpan &out)
{
	const size_t need = kDomainTagLen + 1 /* direction */ + 4 /* counter */ + 4 /* cluster */ +
			    4 /* command */ + 2 + deviceKey.size() + payload.size();

	VerifyOrReturnError(out.size() >= need, CHIP_ERROR_BUFFER_TOO_SMALL);

	uint8_t *p = out.data();

	memcpy(p, kDomainTag, kDomainTagLen);
	p += kDomainTagLen;

	*p++ = direction;

	PutU32(p, counter);
	p += 4;
	PutU32(p, kClusterId);
	p += 4;
	PutU32(p, kCommandId);
	p += 4;

	/* Length-prefix the key so that a shorter key followed by attacker-chosen
	 * payload bytes cannot produce the same preimage as a longer key - the
	 * classic concatenation ambiguity. */
	*p++ = static_cast<uint8_t>(deviceKey.size() >> 8);
	*p++ = static_cast<uint8_t>(deviceKey.size() & 0xFF);
	memcpy(p, deviceKey.data(), deviceKey.size());
	p += deviceKey.size();

	memcpy(p, payload.data(), payload.size());
	p += payload.size();

	out.reduce_size(static_cast<size_t>(p - out.data()));
	return CHIP_NO_ERROR;
}

} // namespace tunnel
