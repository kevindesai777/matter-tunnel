/*
 * Client key registration for the VendorTunnel.
 *
 * The mirror of key distribution (tunnel_identity.h), and the harder half.
 * The device proves itself with its Matter DAC; the client has no equivalent
 * credential, so the device must be told which public key to trust.
 *
 * Why the obvious answers fail
 * ----------------------------
 *   First-write-wins / trust-on-first-use
 *       The hub relays the registration, so a hostile hub simply registers its
 *       own key first and wins. Worse, it wins silently.
 *
 *   Proof of the setup passcode
 *       The hub already knows the passcode - it used it to commission the
 *       device. It can present the same proof.
 *
 *   Authorisation by a Matter fabric administrator
 *       In this threat model the hub *is* the administrator.
 *
 * What is left is the one thing a remote attacker cannot reach: physical
 * possession. This is the same trust root Matter itself uses - commissioning
 * requires either a factory-fresh device or a deliberate physical action to
 * open the window - so the mechanism here mirrors a model users and reviewers
 * already accept, rather than inventing one.
 *
 * The mechanism
 * -------------
 *   1. A button press on the device opens a registration window (60 s).
 *   2. The first key registered inside the window is stored persistently.
 *   3. The device returns the key it actually stored, signed with its tunnel
 *      key - which the client already trusts via the DAC chain.
 *
 * Step 3 is what makes a race detectable rather than silent. If a hostile hub
 * manages to register its own key inside the window, the client sees a signed
 * confirmation naming a key that is not its own, and can tell the user. Without
 * the signed echo the client would believe it had succeeded.
 *
 * Outside the window, registration is refused. Replacing a registered key
 * therefore requires physical access, and a factory reset clears it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "tunnel_crypto.h"

#include <lib/core/CHIPError.h>
#include <lib/support/Span.h>

namespace tunnel {

/* How long a button press keeps registration open. Long enough for a human to
 * complete an app flow, short enough to bound the race window. */
constexpr uint32_t kRegistrationWindowMs = 60000;

class ClientKeyStore {
public:
	/* Load any persisted client key and install the button handler. */
	CHIP_ERROR Init();

	bool HasKey() const { return mKeyLen != 0; }
	chip::ByteSpan Key() const { return chip::ByteSpan(mKey, mKeyLen); }

	bool WindowOpen() const;
	void OpenWindow();

	/* Store `key` if the window is open. Refused otherwise. */
	CHIP_ERROR Register(const chip::ByteSpan &key);

	/* Verify `sig` over `msg` using the registered client key. */
	psa_status_t Verify(const uint8_t *msg, size_t msg_len, const uint8_t *sig, size_t sig_len);

private:
	uint8_t mKey[32] = {}; /* Ed25519 public key */
	size_t mKeyLen = 0;
	int64_t mWindowUntilMs = 0;
	psa_key_id_t mImported = PSA_KEY_ID_NULL;
};

ClientKeyStore &GetClientKeyStore();

} // namespace tunnel
