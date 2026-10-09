// vip_duckspeed unit tests — the engine-free part: the per-slot speed bookkeeping (the
// g_iDuckSpeedCount that lets the per-frame timer return immediately when no VIP is affected)
// and the timer's write condition.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "duckspeed_logic.hpp"

#include <cstdio>
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

} // namespace

int main() {
	std::printf("vip_duckspeed tests\n");

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

	// The write condition: set speed + alive + duck held.
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, vip_duckspeed::kInDuck), true, "ducks with speed");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, 0), false, "no duck button");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, false, vip_duckspeed::kInDuck), false, "dead pawn");
	CheckEq(vip_duckspeed::ShouldWrite(0.0f, true, vip_duckspeed::kInDuck), false, "no stored speed");
	CheckEq(vip_duckspeed::ShouldWrite(1.5f, true, vip_duckspeed::kInDuck | (1ull << 3)), true,
			"other buttons do not matter");

	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
