// vip_duckspeed unit tests — the engine-free part: the per-slot speed bookkeeping (the
// g_iDuckSpeedCount that lets the per-frame work return immediately when no VIP is affected)
// and the write condition of the per-frame update.
//
// Engine-free by design: the header under test includes nothing but the standard library, so this
// binary has no s2sdk, vip or pisexcompat dependency at all. The map gate and the client guards
// live in src/plugin.cpp (they call the plugify/s2sdk bindings and are exercised by the server).
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "duckspeed_logic.hpp"

#include <cstdio>
#include <limits>
#include <string>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEq(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void CheckEq(bool actual, bool expected, const std::string& what) {
	CheckEq(actual ? 1 : 0, expected ? 1 : 0, what);
}

// --- the round policy -------------------------------------------------------------------------

// The exact helper src/plugin.cpp calls immediately before every m_flDuckSpeed write, with the
// production scenarios of the reported bug (the write used to happen on pistol rounds).
void TestRoundPolicy() {
	using vip_duckspeed::kDefaultRoundMin;
	using vip_duckspeed::kRoundMinFeature;
	using vip_duckspeed::RoundAllowed;

	// The exact per-group key the runtime reads from vip/groups.ini.
	CheckEq(std::string(kRoundMinFeature) == "duckspeed_round_min", true, "the groups.ini key is exact");
	CheckEq(kDefaultRoundMin, 1, "the missing-key fallback is 1 (fail closed)");

	// First map before round_prestart: pistol is still false, but no round has completed.
	CheckEq(RoundAllowed(-1, false, 0), false, "missing key: first map before round_prestart is refused");
	CheckEq(RoundAllowed(kDefaultRoundMin, false, 0), false, "fallback 1: total 0 is refused even when not pistol");

	// The first pistol round.
	CheckEq(RoundAllowed(-1, true, 0), false, "missing key: first pistol round is refused");

	// Second regular round (one completed round).
	CheckEq(RoundAllowed(-1, false, 1), true, "missing key: second regular round (total 1) is allowed");

	// Second-half pistol: 12 completed rounds, but VIP_PistolRound is set for the switch.
	CheckEq(RoundAllowed(-1, true, 12), false, "missing key: second-half pistol is refused");

	// Ordinary later rounds.
	CheckEq(RoundAllowed(-1, false, 5), true, "missing key: an ordinary later round is allowed");
	CheckEq(RoundAllowed(1, false, 23), true, "min 1: a later non-pistol round is allowed");

	// 0 explicitly allows every round, the pistol ones and the pre-first-round state included.
	CheckEq(RoundAllowed(0, false, 0), true, "0: the first map is allowed");
	CheckEq(RoundAllowed(0, true, 0), true, "0: the first pistol round is allowed");
	CheckEq(RoundAllowed(0, true, 12), true, "0: the second-half pistol is allowed");

	// A negative total is the SDK's "round state unavailable" answer: no effect may run, not even
	// for an explicit 0 (the check runs before the roundMin == 0 branch).
	CheckEq(RoundAllowed(0, false, -1), false, "unavailable round state (total -1) refuses an explicit 0");
	CheckEq(RoundAllowed(-1, false, -1), false, "unavailable round state refuses the missing-key fallback");

	// A higher minimum counts COMPLETED rounds and never allows a pistol round.
	CheckEq(RoundAllowed(2, false, 1), false, "min 2 waits for the second completed round");
	CheckEq(RoundAllowed(2, false, 2), true, "min 2 allows once two rounds completed");
	CheckEq(RoundAllowed(2, true, 5), false, "min 2 refuses a pistol round");
	CheckEq(RoundAllowed(3, false, 2), false, "min 3 still waits at two completed rounds");
	CheckEq(RoundAllowed(3, false, 3), true, "min 3 allows at three completed rounds");
	CheckEq(RoundAllowed(5, false, 4), false, "min 5 waits at four completed rounds");
	CheckEq(RoundAllowed(5, false, 9), true, "min 5 allows later");

	// Any negative value is treated like the core's missing -1 (the fallback is 1).
	CheckEq(RoundAllowed(-7, false, 0), false, "a negative setting falls back to 1: total 0 refused");
	CheckEq(RoundAllowed(-7, false, 1), true, "a negative setting falls back to 1: total 1 allowed");

	// A huge value merely keeps waiting; no wrap turns it into an early allow.
	CheckEq(RoundAllowed(std::numeric_limits<int>::max(), false, 100), false, "INT_MAX does not wrap");
}

// The spawn caches the threshold next to the speed, and a refused round keeps the spawn value so
// the effect resumes unchanged on an eligible round (nothing is reset to a magic constant).
void TestRoundAwareSpawn() {
	vip_duckspeed::State state;
	vip_duckspeed::OnSpawn(state, 4, true, 1.5f, 2);
	CheckEq(state.roundMin[4], 2, "the spawn caches the group threshold");
	CheckEq(state.speed[4] == 1.5f, true, "the spawn speed is cached");

	CheckEq(vip_duckspeed::RoundAllowed(state.roundMin[4], false, 1), false, "below the threshold the round is refused");
	CheckEq(state.speed[4] == 1.5f, true, "a refused round keeps the cached spawn speed");
	CheckEq(vip_duckspeed::ShouldWrite(state.speed[4], true, vip_duckspeed::kInDuck) &&
			vip_duckspeed::RoundAllowed(state.roundMin[4], false, 2),
			true, "the write resumes on an eligible round with the same value");

	// A missing key is cached as -1 and the fallback (1) applies at write time: the first map
	// (total 0) and a pistol round stay refused; the second regular round is allowed.
	vip_duckspeed::State missing;
	vip_duckspeed::OnSpawn(missing, 6, true, 0.75f, -1);
	CheckEq(missing.roundMin[6], -1, "the missing key is cached as -1");
	CheckEq(vip_duckspeed::RoundAllowed(missing.roundMin[6], false, 0), false, "missing key: total 0 refused");
	CheckEq(vip_duckspeed::RoundAllowed(missing.roundMin[6], false, 1), true, "missing key: total 1 allowed");
	CheckEq(vip_duckspeed::RoundAllowed(missing.roundMin[6], true, 12), false, "missing key: second-half pistol refused");

	// An explicit 0 allows every round (the stored speed is still required).
	vip_duckspeed::State always;
	vip_duckspeed::OnSpawn(always, 7, true, 2.0f, 0);
	CheckEq(vip_duckspeed::RoundAllowed(always.roundMin[7], true, 0), true, "explicit 0 allows the first pistol round");
	CheckEq(vip_duckspeed::RoundAllowed(always.roundMin[7], true, 12), true, "explicit 0 allows the second-half pistol");

	// A group/config switch without a respawn: the spawn-cached 0 passes, the fresh 1 the runtime
	// reads right before the write must deny it (and a fresh 0 must permit again after a switch
	// the other way). The spawn speed value itself is kept either way.
	CheckEq(vip_duckspeed::RoundAllowed(0, true, 12), true, "the cached 0 passes on the second-half pistol");
	CheckEq(vip_duckspeed::RoundAllowed(1, true, 12), false, "the fresh 1 denies the write without a respawn");
	CheckEq(vip_duckspeed::RoundAllowed(0, true, 12), true, "a fresh 0 permits again without a respawn");
	CheckEq(always.speed[7] == 2.0f, true, "the denied switch keeps the spawn speed value");
}

} // namespace

int main() {
	std::printf("vip_duckspeed tests\n");

	TestRoundPolicy();
	TestRoundAwareSpawn();

	// A VIP spawn stores its value and counts it once.
	vip_duckspeed::State state;
	vip_duckspeed::OnSpawn(state, 3, true, 1.2f);
	CheckEq(state.speed[3] > 1.19f && state.speed[3] < 1.21f, true, "speed stored");
	CheckEq(state.count, 1, "counted once");

	// Respawning does not double-count.
	vip_duckspeed::OnSpawn(state, 3, true, 1.2f);
	CheckEq(state.count, 1, "respawn keeps one entry");

	// A non-VIP clears the slot (and uncounts it).
	vip_duckspeed::OnSpawn(state, 3, false, 0.0f);
	CheckEq(state.speed[3] == 0.0f, true, "non-VIP cleared");
	CheckEq(state.count, 0, "uncounted");

	// A VIP with the neutral value (1.0 for a missing group value) stores 1.0, like the original.
	vip_duckspeed::OnSpawn(state, 5, true, 1.0f);
	CheckEq(state.count, 1, "neutral 1.0 is treated as set (the original's > 0 check)");
	CheckEq(state.speed[5] == 1.0f, true, "neutral value stored");

	// A zero/negative feature value clears the slot of a VIP.
	vip_duckspeed::OnSpawn(state, 5, true, 0.0f);
	CheckEq(state.count, 0, "zeroed");
	CheckEq(state.speed[5] == 0.0f, true, "cleared");

	// Slots outside the range are ignored (no out-of-bounds write).
	vip_duckspeed::OnSpawn(state, 64, true, 2.0f);
	vip_duckspeed::OnSpawn(state, -1, true, 2.0f);
	CheckEq(state.count, 0, "out-of-range slots ignored");

	// Slots are independent: one slot's spawn never touches another slot.
	vip_duckspeed::State slots;
	vip_duckspeed::OnSpawn(slots, 7, true, 0.5f);
	vip_duckspeed::OnSpawn(slots, 9, true, 2.0f);
	CheckEq(slots.count, 2, "two VIPs counted");
	CheckEq(slots.speed[7] == 0.5f, true, "slot 7 value kept");
	CheckEq(slots.speed[9] == 2.0f, true, "slot 9 value kept");
	vip_duckspeed::OnSpawn(slots, 7, false, 0.0f);
	CheckEq(slots.count, 1, "clearing one slot leaves the other counted");
	CheckEq(slots.speed[9] == 2.0f, true, "the other slot is untouched");

	// A respawn with a new value replaces it and keeps a single count.
	vip_duckspeed::OnSpawn(slots, 9, true, 1.5f);
	CheckEq(slots.count, 1, "a new value keeps one entry");
	CheckEq(slots.speed[9] == 1.5f, true, "a new value replaces the old");

	// A negative feature value clears the slot, like zero (the original's "> 0" test).
	vip_duckspeed::OnSpawn(slots, 9, true, -3.0f);
	CheckEq(slots.count, 0, "negative clears");
	CheckEq(slots.speed[9] == 0.0f, true, "negative value stored as cleared");

	// The slot range is exactly 0..63: slot 63 is valid, slot 64 is not.
	vip_duckspeed::State edge;
	vip_duckspeed::OnSpawn(edge, 63, true, 1.25f);
	CheckEq(edge.count, 1, "slot 63 is valid");
	CheckEq(edge.speed[63] == 1.25f, true, "slot 63 stored");
	vip_duckspeed::OnSpawn(edge, 64, true, 1.25f);
	CheckEq(edge.count, 1, "slot 64 ignored");

	// The write condition: set speed + alive + duck held.
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, vip_duckspeed::kInDuck), true, "ducks with speed");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, 0), false, "no duck button");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, false, vip_duckspeed::kInDuck), false, "dead pawn");
	CheckEq(vip_duckspeed::ShouldWrite(0.0f, true, vip_duckspeed::kInDuck), false, "no stored speed");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, vip_duckspeed::kInDuck | (1ull << 3)), true,
			"other buttons do not matter");

	// IN_DUCK is the original's button-state bit 1 << 2.
	CheckEq(vip_duckspeed::kInDuck == (1ull << 2), true, "duck bit is 1 << 2");

	// CS2 has 64 player slots; the guards in src/plugin.cpp use the same bound.
	CheckEq(vip_duckspeed::kMaxSlots, 64, "64 slots");

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
