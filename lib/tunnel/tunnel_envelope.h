/*
 * Signed envelope for the VendorTunnel.
 *
 * The signature must cover more than the payload. Signing the payload alone
 * leaves a signed message redirectable to a different command, a different
 * cluster, a different device, or replayable at a later time. The preimage
 * therefore binds all of them:
 *
 *   "MTUNv1"          domain separation - a signature made here can never be
 *                     mistaken for one made by another protocol using the same
 *                     key, and vice versa
 *   u8   direction    request vs response, so a response cannot be replayed
 *                     back as a request
 *   u32  counter      freshness (see tunnel_replay.h)
 *   u32  cluster_id   binds to this cluster
 *   u32  command_id   binds to this command
 *   u8[] device_key   binds to THIS device
 *   ...  payload
 *
 * On device identity: the binding uses the device's own tunnel public key, NOT
 * the Matter operational NodeId. Measured 2026-08-17: removing a device from an
 * ecosystem and re-adding it preserves the fabric but issues a NEW NodeId, so a
 * scheme keyed on NodeId fails silently after a re-pair - the device keeps
 * working until someone re-adds it, which is the worst way for a security
 * mechanism to break. The tunnel key is persistent and device-owned.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>
#include <stdint.h>

namespace tunnel {

constexpr uint8_t kDirRequest = 0x02;
constexpr uint8_t kDirResponse = 0x03;

constexpr uint32_t kClusterId = 0xFFF1FC02;
constexpr uint32_t kCommandId = 0xFFF10000;

/*
 * Build the signature preimage into `out`, reducing it to the bytes written.
 * `deviceKey` is the device's tunnel public key; `payload` the application
 * bytes being protected.
 */
CHIP_ERROR BuildPreimage(uint8_t direction, uint32_t counter, const chip::ByteSpan &deviceKey,
			 const chip::ByteSpan &payload, chip::MutableByteSpan &out);

} // namespace tunnel
