// vip_duckspeed_runtime_tests — the real plugin driven through the generated s2sdk/vip bindings.
//
// The plugin translation unit is compiled into this binary (../src/plugin.cpp), exactly like the
// production .so; the generated binding globals come from the s2sdk_bindings / vip_bindings object
// libraries and only their call targets are replaced by the recording fakes below. The test
// therefore drives the actual OnPluginStart() / OnPluginUpdate() path and the actual captured
// VIP_OnPlayerSpawn delegate, and observes the real per-frame raw write to the fake
// CCSPlayer_MovementServices::m_flDuckSpeed memory.
//
// Covered (round policy):
//   * missing 'duckspeed_round_min' (-1) falls back to 1: no write on the first round, a write on
//     the second non-pistol round;
//   * both pistol rounds (first and second half) are refused at the default threshold;
//   * an explicit 0 permits a pistol round (both halves);
//   * a threshold >= 2 waits for that many completed rounds;
//   * a threshold change 0 -> 1 without a respawn blocks the next write immediately (the
//     production reread-before-effect contract; fails on a spawn-cached implementation);
//   * OnMapEnd blocks the effect and every round/client/schema call;
//   * a missing round binding fails closed, and restoring it resumes the effect.
//
// Run by ./test.sh (build/tests/vip_duckspeed_runtime_tests).

#include "../src/plugin.cpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <string_view>

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckEqInt(int actual, int expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %d, got %d)\n", what.c_str(), expected, actual);
	}
}

void CheckEqFloat(float actual, float expected, const std::string& what) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL: %s (expected %g, got %g)\n", what.c_str(), static_cast<double>(expected),
					static_cast<double>(actual));
	}
}

std::string ToStd(const plg::string& value) {
	return std::string(std::string_view(value));
}

constexpr int kSlots = vip_duckspeed::kMaxSlots;

// --- fake entity memory ----------------------------------------------------------------------
//
// The plugin resolves CBasePlayerPawn::m_pMovementServices and then dereferences the pointer at
// that offset to obtain the movement-services object, where it writes m_flDuckSpeed as a float.
// The fake pawn buffer stores the address of the fake services object; the services buffer is
// where the test observes the actual writes.

constexpr int32_t kMovementServicesOffset = 8;
constexpr int32_t kDuckSpeedOffset = 4;
constexpr float kNoWrite = -1234.5f; // a value no configured speed uses

struct alignas(8) FakeEntity {
	std::array<char, 64> memory{};
};

struct alignas(8) FakeServices {
	std::array<char, 16> memory{};
};

std::array<FakeEntity, static_cast<std::size_t>(kSlots)> g_pawns{};
std::array<FakeServices, static_cast<std::size_t>(kSlots)> g_services{};

std::array<bool, static_cast<std::size_t>(kSlots)> g_connected{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_inGame{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_sourceTV{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_alive{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_hasController{};
std::array<bool, static_cast<std::size_t>(kSlots)> g_hasPawn{};
std::array<uint64_t, static_cast<std::size_t>(kSlots)> g_buttons{};

std::array<std::map<std::string, float, std::less<>>, static_cast<std::size_t>(kSlots)> g_featureFloats;
std::array<std::map<std::string, int, std::less<>>, static_cast<std::size_t>(kSlots)> g_featureInts;

bool g_vipLoaded = true;
bool g_pistolRound = false;
int g_totalRounds = 0;

int g_pistolCalls = 0;
int g_totalRoundsCalls = 0;
int g_clientCalls = 0; // every IsClient*/PlayerSlotToEntPointer/GetClientPawn/GetClientButtons call
int g_schemaCalls = 0;

s2sdk::OnMapStartCallback g_mapStartCallback = nullptr;
s2sdk::OnMapEndCallback g_mapEndCallback = nullptr;
::vip::SpawnDelegate g_spawnDelegate = nullptr;

bool InRange(int32_t slot) {
	return slot >= 0 && slot < kSlots;
}

// --- fake s2sdk exports ----------------------------------------------------------------------

bool FakeIsClientConnected(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_connected[static_cast<std::size_t>(slot)];
}

bool FakeIsClientInGame(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_inGame[static_cast<std::size_t>(slot)];
}

bool FakeIsClientSourceTV(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_sourceTV[static_cast<std::size_t>(slot)];
}

bool FakeIsClientAlive(int32_t slot) {
	++g_clientCalls;
	return InRange(slot) && g_alive[static_cast<std::size_t>(slot)];
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	++g_clientCalls;
	if (!InRange(slot) || !g_hasController[static_cast<std::size_t>(slot)]) {
		return nullptr;
	}
	return g_pawns[static_cast<std::size_t>(slot)].memory.data();
}

void* FakeGetClientPawn(int32_t slot) {
	++g_clientCalls;
	if (!InRange(slot) || !g_hasPawn[static_cast<std::size_t>(slot)]) {
		return nullptr;
	}
	return g_pawns[static_cast<std::size_t>(slot)].memory.data();
}

uint64_t FakeGetClientButtons(int32_t slot, int32_t buttonIndex) {
	++g_clientCalls;
	if (!InRange(slot) || buttonIndex != 0) {
		return 0;
	}
	return g_buttons[static_cast<std::size_t>(slot)];
}

int32_t FakeGetSchemaOffset(const plg::string& className, const plg::string& memberName) {
	++g_schemaCalls;
	const std::string key = ToStd(className) + "::" + ToStd(memberName);
	if (key == "CBasePlayerPawn::m_pMovementServices") {
		return kMovementServicesOffset;
	}
	if (key == "CCSPlayer_MovementServices::m_flDuckSpeed") {
		return kDuckSpeedOffset;
	}
	return -1;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_mapStartCallback = callback;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback callback) {
	if (g_mapStartCallback == callback) {
		g_mapStartCallback = nullptr;
	}
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	g_mapEndCallback = callback;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback callback) {
	if (g_mapEndCallback == callback) {
		g_mapEndCallback = nullptr;
	}
}

// --- fake vip exports ------------------------------------------------------------------------

bool FakeIsVIPLoaded() {
	return g_vipLoaded;
}

bool FakePistolRound() {
	++g_pistolCalls;
	return g_pistolRound;
}

int32_t FakeGetTotalRounds() {
	++g_totalRoundsCalls;
	return g_totalRounds;
}

float FakeGetClientFeatureFloat(int32_t slot, const plg::string& feature) {
	if (!InRange(slot)) {
		return 1.0f;
	}
	const auto& values = g_featureFloats[static_cast<std::size_t>(slot)];
	const auto it = values.find(ToStd(feature));
	return it == values.end() ? 1.0f : it->second; // the core's missing-value default
}

int32_t FakeGetClientFeatureInt(int32_t slot, const plg::string& feature) {
	if (!InRange(slot)) {
		return -1;
	}
	const auto& values = g_featureInts[static_cast<std::size_t>(slot)];
	const auto it = values.find(ToStd(feature));
	return it == values.end() ? -1 : it->second; // -1 = missing key
}

void FakeRegisterFeature(const plg::string&, ::vip::ValueType, ::vip::FeatureType,
						 ::vip::ItemSelectableDelegate, ::vip::ItemTogglableDelegate,
						 ::vip::ItemDisplayDelegate) {
}

void FakeOnPlayerSpawnRegister(::vip::SpawnDelegate callback) {
	g_spawnDelegate = callback;
}

void FakeOnPlayerSpawnUnregister(::vip::SpawnDelegate callback) {
	if (g_spawnDelegate == callback) {
		g_spawnDelegate = nullptr;
	}
}

// --- fixtures --------------------------------------------------------------------------------

void InstallFakes() {
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_IsClientAlive = &FakeIsClientAlive;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_GetClientPawn = &FakeGetClientPawn;
	__s2sdk_GetClientButtons = &FakeGetClientButtons;
	__s2sdk_GetSchemaOffset = &FakeGetSchemaOffset;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;

	__vip_VIP_IsVIPLoaded = &FakeIsVIPLoaded;
	__vip_VIP_PistolRound = &FakePistolRound;
	__vip_VIP_GetTotalRounds = &FakeGetTotalRounds;
	__vip_VIP_GetClientFeatureFloat = &FakeGetClientFeatureFloat;
	__vip_VIP_GetClientFeatureInt = &FakeGetClientFeatureInt;
	__vip_VIP_RegisterFeature = &FakeRegisterFeature;
	__vip_VIP_OnPlayerSpawn_Register = &FakeOnPlayerSpawnRegister;
	__vip_VIP_OnPlayerSpawn_Unregister = &FakeOnPlayerSpawnUnregister;
}

void ResetCounters() {
	g_pistolCalls = 0;
	g_totalRoundsCalls = 0;
	g_clientCalls = 0;
	g_schemaCalls = 0;
}

void ResetFakes() {
	for (int32_t slot = 0; slot < kSlots; ++slot) {
		const auto index = static_cast<std::size_t>(slot);
		g_connected[index] = false;
		g_inGame[index] = false;
		g_sourceTV[index] = false;
		g_alive[index] = false;
		g_hasController[index] = false;
		g_hasPawn[index] = false;
		g_buttons[index] = 0;
		g_featureFloats[index].clear();
		g_featureInts[index].clear();
		g_pawns[index].memory.fill(0);
		g_services[index].memory.fill(0);
		void* services = g_services[index].memory.data();
		std::memcpy(g_pawns[index].memory.data() + kMovementServicesOffset, &services, sizeof(services));
	}
	g_vipLoaded = true;
	g_pistolRound = false;
	g_totalRounds = 0;
	g_mapStartCallback = nullptr;
	g_mapEndCallback = nullptr;
	g_spawnDelegate = nullptr;
	ResetCounters();
	InstallFakes();
}

void MakeAlive(int32_t slot) {
	const auto index = static_cast<std::size_t>(slot);
	g_connected[index] = true;
	g_inGame[index] = true;
	g_alive[index] = true;
	g_hasController[index] = true;
	g_hasPawn[index] = true;
	g_buttons[index] = vip_duckspeed::kInDuck;
}

void SetDuckSpeed(int32_t slot, float speed) {
	g_featureFloats[static_cast<std::size_t>(slot)]["duckspeed"] = speed;
}

void SetRoundMin(int32_t slot, int roundMin) {
	g_featureInts[static_cast<std::size_t>(slot)][vip_duckspeed::kRoundMinFeature] = roundMin;
}

void EraseRoundMin(int32_t slot) {
	g_featureInts[static_cast<std::size_t>(slot)].erase(vip_duckspeed::kRoundMinFeature);
}

void FireSpawn(int32_t slot, bool isVIP) {
	if (g_spawnDelegate != nullptr) {
		g_spawnDelegate(slot, 2, isVIP);
	}
}

void FireMapStart() {
	if (g_mapStartCallback != nullptr) {
		g_mapStartCallback();
	}
}

void FireMapEnd() {
	if (g_mapEndCallback != nullptr) {
		g_mapEndCallback();
	}
}

float DuckSpeedAt(int32_t slot) {
	float value = 0.0f;
	std::memcpy(&value, g_services[static_cast<std::size_t>(slot)].memory.data() + kDuckSpeedOffset,
				sizeof(value));
	return value;
}

void MarkNoWrite(int32_t slot) {
	const float sentinel = kNoWrite;
	std::memcpy(g_services[static_cast<std::size_t>(slot)].memory.data() + kDuckSpeedOffset, &sentinel,
				sizeof(sentinel));
}

void RunUpdate() {
	g_plugin.OnPluginUpdate(std::chrono::milliseconds(16));
}

void StartPlugin() {
	ResetFakes();
	// Idempotent unload of a previous test's instance; the fakes above keep it safe.
	g_plugin.OnPluginEnd();
	g_plugin.OnPluginStart();
	FireMapStart();
}

void StopPlugin() {
	g_plugin.OnPluginEnd();
}

// --- tests -----------------------------------------------------------------------------------

// Missing 'duckspeed_round_min' means -1 and falls back to 1: the first round is refused, the
// second non-pistol round writes the stored speed.
void TestDefaultPolicy() {
	StartPlugin();
	MakeAlive(1);
	SetDuckSpeed(1, 2.5f);
	EraseRoundMin(1);
	FireSpawn(1, true);

	g_pistolRound = false;
	g_totalRounds = 0;
	MarkNoWrite(1);
	ResetCounters();
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(1), kNoWrite, "missing round_min refuses the first round");
	CheckEqInt(g_pistolCalls, 1, "the open gate evaluates the round API");
	CheckEqInt(g_totalRoundsCalls, 1, "the completed-round count is queried");

	g_totalRounds = 1;
	MarkNoWrite(1);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(1), 2.5f, "the second non-pistol round writes the stored duck speed");
	StopPlugin();
}

// The default fallback of 1 excludes both pistol rounds: the first one (total 0) and the
// second-half switch (total 12).
void TestPistolRoundsRefused() {
	StartPlugin();
	MakeAlive(2);
	SetDuckSpeed(2, 3.0f);
	EraseRoundMin(2);
	FireSpawn(2, true);

	g_pistolRound = true;
	g_totalRounds = 0;
	MarkNoWrite(2);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(2), kNoWrite, "the first pistol round writes nothing");

	g_totalRounds = 12;
	MarkNoWrite(2);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(2), kNoWrite, "the second-half pistol round writes nothing");
	StopPlugin();
}

// An explicit 0 removes the round restriction: it permits both pistol halves and the state before
// the first round.
void TestExplicitZeroPermitsPistol() {
	StartPlugin();
	MakeAlive(3);
	SetDuckSpeed(3, 2.25f);
	SetRoundMin(3, 0);
	FireSpawn(3, true);

	g_pistolRound = true;
	g_totalRounds = 0;
	MarkNoWrite(3);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(3), 2.25f, "explicit 0 writes on the first pistol round");

	g_totalRounds = 12;
	MarkNoWrite(3);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(3), 2.25f, "explicit 0 writes on the second-half pistol round");
	StopPlugin();
}

// A higher threshold counts completed rounds and never permits a pistol round.
void TestHigherThreshold() {
	StartPlugin();
	MakeAlive(4);
	SetDuckSpeed(4, 1.5f);
	SetRoundMin(4, 2);
	FireSpawn(4, true);

	g_pistolRound = false;
	g_totalRounds = 1;
	MarkNoWrite(4);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(4), kNoWrite, "round_min 2 waits at one completed round");

	g_totalRounds = 2;
	MarkNoWrite(4);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(4), 1.5f, "round_min 2 writes after two completed rounds");

	g_pistolRound = true; // the same completed-round count, but a pistol round
	MarkNoWrite(4);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(4), kNoWrite, "round_min 2 still refuses a pistol round");
	StopPlugin();
}

// The production contract: the threshold is read at the effect, not only at spawn, so a group
// change 0 -> 1 blocks the very next write without a respawn (and before any 1 s refresh).
void TestThresholdChangeWithoutRespawn() {
	StartPlugin();
	MakeAlive(5);
	SetDuckSpeed(5, 2.75f);
	SetRoundMin(5, 0);
	g_pistolRound = true;
	g_totalRounds = 0;
	FireSpawn(5, true);

	MarkNoWrite(5);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(5), 2.75f, "explicit 0 writes on a pistol round before the change");

	SetRoundMin(5, 1); // no respawn, no spawn callback
	MarkNoWrite(5);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(5), kNoWrite,
				 "a 0 -> 1 threshold change without a respawn blocks the next write immediately");
	StopPlugin();
}

// OnMapEnd closes the gate: no round API, client, schema or memory traffic may happen.
void TestMapEndBlocksEverything() {
	StartPlugin();
	MakeAlive(6);
	SetDuckSpeed(6, 1.75f);
	EraseRoundMin(6);
	g_pistolRound = false;
	g_totalRounds = 1;
	FireSpawn(6, true);

	MarkNoWrite(6);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(6), 1.75f, "the effect works before the map ends");

	FireMapEnd();
	MarkNoWrite(6);
	ResetCounters();
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(6), kNoWrite, "the map-end window writes nothing");
	CheckEqInt(g_pistolCalls, 0, "the map-end window does not query the round API");
	CheckEqInt(g_totalRoundsCalls, 0, "the map-end window does not read the completed rounds");
	CheckEqInt(g_clientCalls, 0, "the map-end window makes no client call");
	CheckEqInt(g_schemaCalls, 0, "the map-end window makes no schema call");
	StopPlugin();
}

// A missing round binding must not enable the effect (fail closed); restoring it resumes the
// stored speed without a respawn.
void TestMissingRoundApiFailsClosed() {
	StartPlugin();
	MakeAlive(7);
	SetDuckSpeed(7, 2.0f);
	EraseRoundMin(7);
	g_pistolRound = false;
	g_totalRounds = 1;
	FireSpawn(7, true);

	MarkNoWrite(7);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(7), 2.0f, "the effect works with the round API available");

	__vip_VIP_PistolRound = nullptr;
	MarkNoWrite(7);
	ResetCounters();
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(7), kNoWrite, "a missing round binding fails closed");
	CheckEqInt(g_pistolCalls, 0, "no pistol-round call is made when the API is incomplete");
	CheckEqInt(g_totalRoundsCalls, 0, "no round call is made when the API is incomplete");
	CheckEqInt(g_clientCalls, 0, "no client call is made when the round API is incomplete");
	CheckEqInt(g_schemaCalls, 0, "no schema call is made when the round API is incomplete");

	__vip_VIP_PistolRound = &FakePistolRound;
	MarkNoWrite(7);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(7), 2.0f, "restoring the round binding resumes the effect");
	StopPlugin();
}

// VIP_IsVIPLoaded() == false must fail closed even with an otherwise eligible client and round;
// restoring the core resumes the stored speed without a respawn. Slot 8 is otherwise unused.
void TestCoreNotLoadedFailsClosed() {
	StartPlugin();
	MakeAlive(8);
	SetDuckSpeed(8, 2.0f);
	EraseRoundMin(8);
	g_pistolRound = false;
	g_totalRounds = 1;
	FireSpawn(8, true);

	MarkNoWrite(8);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(8), 2.0f, "the effect works with the VIP core loaded");

	g_vipLoaded = false;
	MarkNoWrite(8);
	ResetCounters();
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(8), kNoWrite, "VIP_IsVIPLoaded false writes no duck speed");
	CheckEqInt(g_pistolCalls, 0, "no round call is made with the core unloaded");
	CheckEqInt(g_totalRoundsCalls, 0, "no completed-round read is made with the core unloaded");
	CheckEqInt(g_clientCalls, 0, "no client call is made with the core unloaded");
	CheckEqInt(g_schemaCalls, 0, "no schema call is made with the core unloaded");

	g_vipLoaded = true;
	MarkNoWrite(8);
	RunUpdate();
	CheckEqFloat(DuckSpeedAt(8), 2.0f, "restoring the VIP core resumes the effect");
	StopPlugin();
}

} // namespace

int main() {
	std::printf("vip_duckspeed runtime tests\n");
	TestDefaultPolicy();
	TestPistolRoundsRefused();
	TestExplicitZeroPermitsPistol();
	TestHigherThreshold();
	TestThresholdChangeWithoutRespawn();
	TestMapEndBlocksEverything();
	TestMissingRoundApiFailsClosed();
	TestCoreNotLoadedFailsClosed();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
