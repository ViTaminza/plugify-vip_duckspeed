// vip_duckspeed — Plugify port of the Pisex VIP module "VIP_DuckSpeed" / "[VIP] DuckSpeed"
// (the original's vip_duckspeed.cpp, 172 lines).
//
// Behaviour kept identical:
//   * feature "duckspeed" (VIP_FLOAT, HIDE).
//   * a repeating every-tick timer (Pisex's CreateTimer(0.0f, DuckSpeedTimer) with the callback
//     returning 0.0f, i.e. every frame) writes m_flDuckSpeed = <stored value> to every alive VIP
//     whose slot has a stored value and who is holding the duck button. On s2sdk the interval
//     must be positive (see kFrameInterval) - a 0 s repeat timer spins TimerSystem::RunFrame
//     forever and the engine watchdog kills the server.
//   * VIP_OnPlayerSpawn stores (or clears) the slot value and keeps the g_iDuckSpeedCount the
//     timer uses to return early when no slot is set.
//
// Port notes:
//   * the original's controller + pawn + alive chain becomes pisex::IsAlive, and its duck-button
//     test on the movement-services buttons becomes s2sdk::GetClientButtons.
//   * m_flDuckSpeed has no s2sdk helper: it is written through the schema on the raw
//     movement-services pointer (CBasePlayerPawn::m_pMovementServices ->
//     CCSPlayer_MovementServices). The original wrote the field through its reference, which does
//     *not* flag a network state change, so the port writes with changeState = false as well. The
//     two schema offsets are resolved once and the timer writes the float directly (same bytes,
//     without the per-call class+field string lookups of pisex::SchemaOffset + SetEntSchemaFloat2).
//   * the original's StartupServer reset (entity-system pointers + clearing the table) is not
//     needed: the port keeps no engine pointers and its state starts empty.
//   * the per-frame loop skips slots without a stored speed before the pawn queries, which is
//     exactly when the original's checks could not have written anything either.

#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>
#include <vip_api.hpp>

#include "duckspeed_logic.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

// The original code's unqualified spellings (VIP_FLOAT, HIDE, ...).
using namespace vip;

constexpr const char* kLogTag = "VIP-DuckSpeed"; // GetLogTag() of the original
constexpr const char* kFeature = "duckspeed";

IVIPApi* g_pVIPCore = &IVIPApi::Get();

void LogInfo(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Info, plg::source_location::current());
	}
}

void LogError(std::string_view message) {
	if (plg::Log) {
		plg::Log(message, plg::Severity::Error, plg::source_location::current());
	}
}

// The original's ErrorLog("[%s] ...", GetLogTag()).
void LogTaggedError(const std::string& message) {
	LogError(std::string("[") + kLogTag + "] " + message);
}

// The original's CreateTimer(0.0f, ...) ran every frame. A 0 second delay is FATAL on s2sdk:
// TimerSystem::RunFrame() re-inserts a repeat timer with executeTime = universalTime + delay and
// keeps looping while any timer is due, so a 0 delay stays due forever (the server frame never
// returns -> "WatchDog! Server took too long to process" -> fatal). 1 ms is below one engine tick
// (0.015625 s), so the timer still runs exactly once per tick, and a reschedule with 1 ms is
// never due again within the same RunFrame pass.
constexpr float kFrameInterval = 0.001f;
static_assert(kFrameInterval > 0.0f, "a 0 second interval hangs s2sdk's TimerSystem::RunFrame loop");

// The two schema offsets of the timer are resolved once and cached: pisex::SchemaOffset builds a
// std::string key and does an unordered_map lookup per call, and s2sdk::SetEntSchemaFloat2 does a
// plg::string conversion, a bad-list hash lookup, a schema map lookup and a type check per call.
// The timer paid both per ducking VIP per tick; the lookups of a fixed class+field never change.
// -1 (schema not up yet / field missing) is retried, exactly like pisexcompat's own cache.
int MovementServicesOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset("CBasePlayerPawn", "m_pMovementServices");
	}
	return offset;
}

int DuckSpeedOffset() {
	static int offset = -1;
	if (offset < 0) {
		offset = pisex::SchemaOffset("CCSPlayer_MovementServices", "m_flDuckSpeed");
	}
	return offset;
}

// The pawn's movement services (the original's m_pMovementServices), read from the raw pawn
// pointer; the component pointer has no s2sdk helper.
void* MovementServices(int iSlot) {
	// s2sdk's GetClientPawn warns "invalid player slot: N" (to the server console) for every call
	// on a slot without a controller, so an empty slot is filtered out here.
	if (!__s2sdk_GetClientPawn || iSlot < 0 || iSlot >= vip_duckspeed::kMaxSlots || !pisex::IsConnected(iSlot)) {
		return nullptr;
	}
	const int offset = MovementServicesOffset();
	if (offset < 0) {
		return nullptr;
	}
	try {
		void* pawn = s2sdk::GetClientPawn(iSlot);
		if (!pawn) {
			return nullptr;
		}
		// m_pMovementServices is a pointer field: its value is the component address.
		return *reinterpret_cast<void**>(static_cast<char*>(pawn) + offset);
	} catch (...) {
		return nullptr;
	}
}

class DuckSpeedPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

private:
	// VIP_OnPlayerSpawn of the original.
	void OnPlayerSpawn(int iSlot, int iTeam, bool bIsVIP);
	// DuckSpeedTimer of the original: returns 0.0, i.e. it runs again next frame.
	float DuckSpeedTimer();

	vip_duckspeed::State m_state;
	pisex::CTimer* m_timer = nullptr;
};

DuckSpeedPlugin g_plugin;

float DuckSpeedPlugin::DuckSpeedTimer() {
	// The original's early-out: no entity system, or no slot with a stored speed.
	// The schema offsets resolve through pisex::SchemaOffset (a -1 skips the write, see the
	// offset helpers); the s2sdk bindings used here are the two client queries.
	if (m_state.count <= 0 || !__s2sdk_GetClientButtons || !__s2sdk_GetClientPawn) {
		return kFrameInterval; // keep the timer alive; never 0.0f (see kFrameInterval)
	}

	for (int i = 0; i < vip_duckspeed::kMaxSlots; ++i) {
		// Slots without a positive stored speed are skipped.
		if (m_state.speed[i] <= 0.0f) {
			continue;
		}
		// CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive.
		const bool bAlive = pisex::IsAlive(i);
		unsigned long long nButtons = 0;
		if (bAlive) {
			try {
				nButtons = s2sdk::GetClientButtons(i, 0); // m_nButtons().m_pButtonStates()[0]
			} catch (...) {
				nButtons = 0;
			}
		}
		if (!vip_duckspeed::ShouldWrite(m_state.speed[i], bAlive, nButtons)) {
			continue;
		}
		void* services = MovementServices(i);
		if (!services) {
			continue;
		}
		// The original's duck-speed write, at the cached offset with changeState = false (the field
		// is a float; the original's field-reference write marked no network state either). The same
		// bytes SetEntSchemaFloat2 writes, without the per-call class+field string lookups.
		const int duckOffset = DuckSpeedOffset();
		if (duckOffset < 0) {
			continue;
		}
		try {
			*reinterpret_cast<float*>(static_cast<char*>(services) + duckOffset) = m_state.speed[i];
		} catch (...) {
		}
	}
	return kFrameInterval; // repeats every frame (the Pisex CreateTimer(0.0f, ...) of the original)
}

void DuckSpeedPlugin::OnPlayerSpawn(int iSlot, int /*iTeam*/, bool bIsVIP) {
	// The original read the feature for a VIP and used 0 for everyone else.
	const float fDuckSpeed =
		bIsVIP ? g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kFeature) : 0.0f;
	vip_duckspeed::OnSpawn(m_state, iSlot, bIsVIP, fDuckSpeed);
}

plg::PluginResult DuckSpeedPlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("vip_duckspeed");

		// The original created the every-frame timer in AllPluginsLoaded (0.0f would hang the
		// server on s2sdk - see kFrameInterval).
		m_timer = pisex::CreateTimer(kFrameInterval, [this]() { return DuckSpeedTimer(); });

		// The original registered the spawn callback in AllPluginsLoaded.
		g_pVIPCore->VIP_OnPlayerSpawn([this](int iSlot, int iTeam, bool bIsVIP) {
			OnPlayerSpawn(iSlot, iTeam, bIsVIP);
		});

		// The original registered the "duckspeed" feature (float, hidden) in AllPluginsLoaded.
		g_pVIPCore->VIP_RegisterFeature(kFeature, VIP_FLOAT, HIDE);

		LogInfo(std::string("[") + kLogTag + "] ready");
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult DuckSpeedPlugin::OnPluginUpdate(std::chrono::milliseconds /*dt*/) {
	try {
		pisex::Poll(); // timer reschedules
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult DuckSpeedPlugin::OnPluginEnd() {
	try {
		if (m_timer != nullptr) {
			pisex::RemoveTimer(m_timer);
			m_timer = nullptr;
		}
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();
		pisex::Shutdown();
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
