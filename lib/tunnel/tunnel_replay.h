/*
 * Anti-replay for the VendorTunnel.
 *
 * A signature proves origin, not freshness: a hostile hub can capture a signed
 * "unlock" and present it again later. For a lock this is the attack, so replay
 * protection is not optional.
 *
 * Why a counter and not challenge-response
 * ----------------------------------------
 * Challenge-response is the textbook answer and needs no persistent state, but
 * it costs an extra round trip. Measured on this bench that is ~250 ms - about
 * six times the entire signing cost (~40 ms sign+verify). Paying 250 ms to
 * protect a 40 ms operation would also inflate the very latency the evaluation
 * is trying to attribute to signing. A counter costs zero round trips and a few
 * bytes.
 *
 * The flaw a naive counter has, and the fix
 * -----------------------------------------
 * Storing "highest counter seen" and persisting it on every command is both
 * slow (an NVS write per command) and unsafe: if the device reboots after
 * accepting counters that were never persisted, the stored value REGRESSES and
 * those counters become acceptable again - reopening exactly the replay window
 * the counter exists to close.
 *
 * So we persist a *reserved ceiling* rather than the current value:
 *
 *   - on boot, the accept floor is set to the persisted ceiling;
 *   - counters are accepted while floor < counter <= ceiling, updating the
 *     floor in RAM only;
 *   - when a counter reaches the ceiling, a new ceiling (ceiling + kReserve) is
 *     persisted before the counter is accepted.
 *
 * The floor therefore never moves backwards across a reboot, and NVS is written
 * once per kReserve commands instead of once per command. The cost is that a
 * reboot burns up to kReserve counter values - harmless, because the client
 * simply moves past them.
 *
 * Client resynchronisation
 * ------------------------
 * A rejected command returns the device's current floor, so a client whose
 * counter has fallen behind (a reinstall, a restored backup) can jump past it
 * and retry. No separate resync exchange is needed: the rejection carries the
 * information. Steady state costs nothing; only the rare mismatch costs a round
 * trip.
 *
 * Scope limitation
 * ----------------
 * One counter space, therefore one client. Multiple independent clients would
 * interfere, each rejecting the other's counters. Production use needs a
 * counter per client keyed by the client's public key; the structure here
 * extends to that without changing the wire format.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <lib/core/CHIPError.h>
#include <stdint.h>

namespace tunnel {

/* Counters reserved per persisted write. Larger = fewer flash writes, more
 * counter values burned per reboot. */
constexpr uint32_t kReserve = 64;

class ReplayGuard {
public:
	/* Load the persisted ceiling and set the accept floor to it. */
	CHIP_ERROR Init();

	/* True if `counter` is acceptable (strictly greater than the floor). */
	bool IsFresh(uint32_t counter) const { return counter > mFloor; }

	/* Record `counter` as used, extending and persisting the ceiling if
	 * required. Call only after the signature has verified, so an unsigned
	 * peer cannot drive the counter - or the flash - forward. */
	CHIP_ERROR Accept(uint32_t counter);

	/* Current floor; returned to a client whose counter was stale. */
	uint32_t Floor() const { return mFloor; }
	uint32_t Ceiling() const { return mCeiling; }

private:
	CHIP_ERROR PersistCeiling(uint32_t ceiling);

	uint32_t mFloor = 0;
	uint32_t mCeiling = 0;
	bool mPersistent = false;
};

} // namespace tunnel
