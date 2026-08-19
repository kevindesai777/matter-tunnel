/*
 * Reserved-ceiling replay guard. See tunnel_replay.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "tunnel_replay.h"

#include <errno.h>
#include <lib/support/logging/CHIPLogging.h>
#include <zephyr/settings/settings.h>

namespace tunnel {
namespace {

constexpr const char *kSettingsKey = "tunnel/ceiling";

uint32_t gLoaded = 0;
bool gLoadedValid = false;

int LoadCb(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg, void *param)
{
	(void)name;
	(void)param;
	if (len != sizeof(uint32_t)) {
		return -EINVAL;
	}
	if (read_cb(cb_arg, &gLoaded, sizeof(gLoaded)) != sizeof(gLoaded)) {
		return -EIO;
	}
	gLoadedValid = true;
	return 0;
}

} // namespace

CHIP_ERROR ReplayGuard::Init()
{
	gLoaded = 0;
	gLoadedValid = false;

	/* Matter initialises the settings subsystem; calling again is harmless
	 * and keeps this component self-contained. */
	int rc = settings_subsys_init();
	if (rc != 0) {
		ChipLogError(Zcl, "tunnel-replay: settings_subsys_init failed (%d); "
				  "counter will NOT survive reboot",
			     rc);
		mFloor = 0;
		mCeiling = kReserve;
		mPersistent = false;
		return CHIP_NO_ERROR;
	}

	mPersistent = true;
	rc = settings_load_subtree_direct(kSettingsKey, LoadCb, nullptr);
	if (rc != 0) {
		ChipLogError(Zcl, "tunnel-replay: load failed (%d)", rc);
	}

	/* The floor starts at the persisted ceiling, never below it. Anything at
	 * or below has to be treated as possibly-used, because commands may have
	 * been accepted in RAM and lost to a reset. */
	mCeiling = gLoadedValid ? gLoaded : 0;
	mFloor = mCeiling;

	if (mCeiling == 0) {
		/* First boot: reserve an initial window so the first commands do
		 * not each trigger a write. */
		ReturnErrorOnFailure(PersistCeiling(kReserve));
	}

	ChipLogProgress(Zcl, "tunnel-replay: floor=%u ceiling=%u persistent=%s",
			static_cast<unsigned>(mFloor), static_cast<unsigned>(mCeiling),
			mPersistent ? "yes" : "NO");
	return CHIP_NO_ERROR;
}

CHIP_ERROR ReplayGuard::PersistCeiling(uint32_t ceiling)
{
	mCeiling = ceiling;

	if (!mPersistent) {
		return CHIP_NO_ERROR;
	}

	int rc = settings_save_one(kSettingsKey, &ceiling, sizeof(ceiling));
	if (rc != 0) {
		ChipLogError(Zcl, "tunnel-replay: persist failed (%d)", rc);
		return CHIP_ERROR_PERSISTED_STORAGE_FAILED;
	}
	return CHIP_NO_ERROR;
}

CHIP_ERROR ReplayGuard::Accept(uint32_t counter)
{
	if (!IsFresh(counter)) {
		return CHIP_ERROR_INVALID_ARGUMENT;
	}

	/* Extend the reservation *before* accepting, so a reset between the two
	 * can only lose the acceptance, never the reservation. Losing the
	 * acceptance is safe; losing the reservation would not be. */
	if (counter >= mCeiling) {
		const uint32_t next = counter + kReserve;
		/* Guard the wrap. A 32-bit counter at even 10 commands/second
		 * lasts ~13 years, so this is a correctness backstop rather than
		 * an expected path - but silently wrapping would reopen replay. */
		if (next < counter) {
			ChipLogError(Zcl, "tunnel-replay: counter space exhausted");
			return CHIP_ERROR_INTERNAL;
		}
		ReturnErrorOnFailure(PersistCeiling(next));
	}

	mFloor = counter;
	return CHIP_NO_ERROR;
}

} // namespace tunnel
