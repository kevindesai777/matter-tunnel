/*
 * Client key registration, gated on physical presence. See tunnel_client_key.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_client_key.h"

#include <errno.h>
#include <lib/support/CodeUtils.h>
#include <lib/support/logging/CHIPLogging.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <dk_buttons_and_leds.h>

using namespace chip;

namespace tunnel {
namespace {

constexpr const char *kSettingsKey = "tunnel/clientkey";

/* Button 4 on the nRF52840 DK. Buttons 1-3 are taken by the upstream
 * application (commissioning/factory reset, lock toggle, DFU), so this handler
 * is additive: registered via dk_button_handler_add() rather than by modifying
 * the application, which keeps this component drop-in. */
constexpr uint32_t kRegisterButtonMask = DK_BTN4_MSK;

uint8_t gLoadedKey[32];
size_t gLoadedLen = 0;

int LoadCb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	(void)name;
	(void)param;
	if (len != sizeof(gLoadedKey)) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, gLoadedKey, sizeof(gLoadedKey)) != (ssize_t)sizeof(gLoadedKey)) {
		return -EIO;
	}
	gLoadedLen = sizeof(gLoadedKey);
	return 0;
}

void ButtonHandler(uint32_t button_state, uint32_t has_changed)
{
	if ((kRegisterButtonMask & has_changed) & button_state) {
		GetClientKeyStore().OpenWindow();
	}
}

struct button_handler gButtonHandler = {
	.cb = ButtonHandler,
};

ClientKeyStore gStore;

} // namespace

ClientKeyStore &GetClientKeyStore()
{
	return gStore;
}

CHIP_ERROR ClientKeyStore::Init()
{
	gLoadedLen = 0;

	if (settings_subsys_init() == 0) {
		settings_load_subtree_direct(kSettingsKey, LoadCb, nullptr);
	}

	if (gLoadedLen == sizeof(mKey)) {
		memcpy(mKey, gLoadedKey, sizeof(mKey));
		mKeyLen = sizeof(mKey);
	}

	dk_button_handler_add(&gButtonHandler);

	ChipLogProgress(Zcl, "tunnel-clientkey: %s (press Button 4 to open a %u s "
			     "registration window)",
			mKeyLen ? "key registered" : "NO key registered",
			static_cast<unsigned>(kRegistrationWindowMs / 1000));
	return CHIP_NO_ERROR;
}

bool ClientKeyStore::WindowOpen() const
{
	return mWindowUntilMs != 0 && k_uptime_get() < mWindowUntilMs;
}

void ClientKeyStore::OpenWindow()
{
	mWindowUntilMs = k_uptime_get() + kRegistrationWindowMs;
	ChipLogProgress(Zcl, "tunnel-clientkey: registration window OPEN for %u s",
			static_cast<unsigned>(kRegistrationWindowMs / 1000));
}

CHIP_ERROR ClientKeyStore::Register(const ByteSpan &key)
{
	/* Physical presence is the whole security argument here; refusing
	 * outside the window is what stops a hub registering its own key. */
	VerifyOrReturnError(WindowOpen(), CHIP_ERROR_ACCESS_DENIED);
	VerifyOrReturnError(key.size() == sizeof(mKey), CHIP_ERROR_INVALID_ARGUMENT);

	memcpy(mKey, key.data(), sizeof(mKey));
	mKeyLen = sizeof(mKey);

	/* Drop any imported handle so the next verify re-imports the new key. */
	if (mImported != PSA_KEY_ID_NULL) {
		psa_destroy_key(mImported);
		mImported = PSA_KEY_ID_NULL;
	}

	int rc = settings_save_one(kSettingsKey, mKey, sizeof(mKey));
	if (rc != 0) {
		ChipLogError(Zcl, "tunnel-clientkey: persist failed (%d)", rc);
	}

	/* Close the window immediately: one registration per press. Leaving it
	 * open would let a racing hub overwrite the key just registered. */
	mWindowUntilMs = 0;

	ChipLogProgress(Zcl, "tunnel-clientkey: registered client key, window closed");
	return CHIP_NO_ERROR;
}

psa_status_t ClientKeyStore::Verify(const uint8_t *msg, size_t msg_len, const uint8_t *sig,
				    size_t sig_len)
{
	if (mKeyLen == 0) {
		return PSA_ERROR_BAD_STATE;
	}

	if (mImported == PSA_KEY_ID_NULL) {
		psa_status_t st = ImportPublicKey(Alg::Ed25519, mKey, mKeyLen, &mImported);
		if (st != PSA_SUCCESS) {
			ChipLogError(Zcl, "tunnel-clientkey: import failed: %d",
				     static_cast<int>(st));
			return st;
		}
	}

	return VerifyWith(mImported, Alg::Ed25519, msg, msg_len, sig, sig_len);
}

} // namespace tunnel
