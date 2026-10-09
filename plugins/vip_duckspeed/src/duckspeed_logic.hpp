// vip_duckspeed — engine-free part of the DuckSpeed module: the per-slot bookkeeping of the
// original (ref/src/cs2-vip-modules/VIP_DuckSpeed/vip_duckspeed.cpp, g_flDuckSpeed /
// g_iDuckSpeedCount and the timer's write condition).
//
// Kept pure so the bookkeeping is unit-testable (tests/vip_duckspeed_tests.cpp).

#pragma once

namespace vip_duckspeed {

constexpr int kMaxSlots = 64;

// IN_DUCK of the CS2 button states (the original's `pButtons[0] & IN_DUCK`).
constexpr unsigned long long kInDuck = 1ull << 2;

// g_flDuckSpeed[64] + g_iDuckSpeedCount of the original.
struct State {
	float speed[kMaxSlots]{};
	int count = 0;
};

// VIP_OnPlayerSpawn of the original: the old value is uncounted first, then the VIP's
// "duckspeed" feature value (a float; the core answers 1.0 for a missing group value) is stored
// when it is > 0, and cleared otherwise. A non-VIP always clears the slot.
inline void OnSpawn(State& state, int iSlot, bool bIsVIP, float fDuckSpeed) {
	if (iSlot < 0 || iSlot >= kMaxSlots) {
		return;
	}
	if (state.speed[iSlot] > 0.0f) {
		state.count--;
	}
	if (bIsVIP) {
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
