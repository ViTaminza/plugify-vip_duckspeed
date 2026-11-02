// vip_duckspeed — engine-free part of the DuckSpeed module: the per-slot bookkeeping of the
// original (vip_duckspeed.cpp: the per-slot speed table, its count and the timer's write
// condition).
//
// Kept pure so the bookkeeping is unit-testable (tests/vip_duckspeed_tests.cpp).

#pragma once

namespace vip_duckspeed {

constexpr int kMaxSlots = 64;

// IN_DUCK of the CS2 button states (the original's button-state bit test).
constexpr unsigned long long kInDuck = 1ull << 2;

// --- the round policy -------------------------------------------------------------------------
//
// The module's optional per-group key in the existing vip/groups.ini (read through the core's
// VIP_GetClientFeatureInt). The key follows the vip_bf/vip_items "round_min" convention, with a
// fail-closed floor for this module:
//   * missing key / no VIP record: the core answers -1, and the fallback is 1 - a missing setting
//     must not rewrite duck speed on the first (pistol) round, which is the reported bug;
//   * 0 explicitly allows every round (the stored speed is still required);
//   * N >= 1 allows only a non-pistol round with at least N completed rounds (m_totalRoundsPlayed,
//     the same threshold vip_bf/vip_items compare). The >= 1 requirement makes the first map fail
//     closed: before round_prestart the pistol flag is false and no round has completed.
//     VIP_PistolRound() is authoritative for both pistol rounds (first and second half).
//   * a negative totalRounds is the SDK's "round state unavailable" answer (-1) and refuses the
//     effect even for an explicit 0: with no trustworthy round answer nothing may be enabled.
inline constexpr char kRoundMinFeature[] = "duckspeed_round_min";
inline constexpr int kDefaultRoundMin = 1;

constexpr bool RoundAllowed(int roundMin, bool pistolRound, int totalRounds) {
	if (totalRounds < 0) {
		return false;
	}
	const int minRounds = roundMin < 0 ? kDefaultRoundMin : roundMin;
	return minRounds == 0 || (!pistolRound && totalRounds >= minRounds);
}

// g_flDuckSpeed[64] + g_iDuckSpeedCount of the original, plus the cached per-group round
// threshold. The spawn speed value is kept while a round refuses the effect, so the effect
// resumes with the same value on the next eligible round (the engine's ordinary crouch speed is
// never written or reset by the plugin).
struct State {
	float speed[kMaxSlots]{};
	int roundMin[kMaxSlots]{};
	int count = 0;
};

// VIP_OnPlayerSpawn of the original: the old value is uncounted first, then the VIP's
// "duckspeed" feature value (a float; the core answers 1.0 for a missing group value) is stored
// when it is > 0, and cleared otherwise. A non-VIP always clears the slot.
//
// iRoundMin is the slot's 'duckspeed_round_min' group key (-1 when the key is missing): it is
// seeded next to the speed at the same moment. The runtime re-reads the key fresh for every slot
// that is about to be written and refreshes this value, then evaluates RoundAllowed() against the
// live frame - the cache never stores an "allowed" decision and never decides on its own.
inline void OnSpawn(State& state, int iSlot, bool bIsVIP, float fDuckSpeed,
					int iRoundMin = kDefaultRoundMin) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	if (state.speed[iSlot] > 0.0f) {
		state.count--;
	}
	if (bIsVIP) {
		state.roundMin[iSlot] = iRoundMin;
		if (fDuckSpeed > 0.0f) {
			state.speed[iSlot] = fDuckSpeed;
			state.count++;
		} else {
			state.speed[iSlot] = 0.0f;
		}
	} else {
		state.speed[iSlot] = 0.0f;
	}
}

// The timer's write condition: only a slot with a stored speed, a living pawn and the duck
// button held gets its m_flDuckSpeed written.
inline bool ShouldWrite(float fSpeed, bool bAlive, unsigned long long nButtons) {
	return fSpeed > 0.0f && bAlive && (nButtons & kInDuck) != 0;
}

} // namespace vip_duckspeed
