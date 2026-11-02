// vip_duckspeed — Plugify port of the Pisex VIP module "VIP_DuckSpeed" / "[VIP] DuckSpeed"
// (the original's vip_duckspeed.cpp, 172 lines).
//
// Behaviour kept identical:
//   * feature "duckspeed" (VIP_FLOAT, HIDE).
//   * every game frame, for each slot with a stored speed whose pawn is alive and who is holding
//     the duck button, m_flDuckSpeed is written the stored value.
//   * VIP_OnPlayerSpawn stores (or clears) the slot value and keeps the count that lets the
//     per-frame work return early when no slot is set.
//
// Pure s2sdk: no s2guard and no pisexcompat (docs/PLUGIN_STANDARD.md §7). Every safety rule the
// module used to get from pisexcompat lives in this file:
//   * PITFALLS B1/B2 - the map gate g_mapReady. No client, entity or schema call is made before
//     the first map or between OnMapEnd and the next OnMapStart: s2sdk dereferences null before
//     the first map and reads an emptied client list during map shutdown. The map listeners are
//     registered in OnPluginStart and removed in OnPluginEnd; readiness is latched by OnMapStart
//     and by the first OnPluginUpdate of a plugin loaded into a running map (unless an OnMapEnd
//     was seen since OnPluginStart), and dropped by OnMapEnd.
//   * PITFALLS C2 - IsConnected/IsAlive: slot 0..63, IsClientConnected, IsClientInGame,
//     !IsClientSourceTV, the controller entity present, IsClientAlive. Every call is guarded by
//     its __s2sdk_* binding too, so a plugin running without s2sdk fails soft instead of calling
//     through a null slot.
//   * PITFALLS C8 - SchemaOffset() answers -1 while the gate is closed (GetSchemaOffset before a
//     map exists segfaults inside the schema system) and caches only successful offsets; the
//     cache is cleared in OnPluginEnd.
//   * PITFALLS C6 - m_flDuckSpeed has no s2sdk helper: it is written at the schema offset on the
//     raw CCSPlayer_MovementServices pointer (CBasePlayerPawn::m_pMovementServices), which is a
//     component rather than an entity, so the raw pointer/offset path is the correct one. The
//     original wrote the field through its reference, which flags no network state change either,
//     so the port keeps the write raw and un-flagged.
//   * PITFALLS C9/B4 - there is no timer at all. The original's CreateTimer(0.0f, ...) callback
//     ran every frame, but a 0 s repeat timer on s2sdk stays due forever inside
//     TimerSystem::RunFrame and the engine watchdog kills the server. The Plugify host already
//     calls OnPluginUpdate once per frame, so TickAllPlayers() runs directly from there - the
//     same point in the frame as pisexcompat's own per-frame pump, with no timer object to leak
//     or to fire into a map change.
//
// Port notes:
//   * the original's controller + pawn + alive chain becomes IsAlive(), and its duck-button test
//     on the movement-services buttons becomes s2sdk::GetClientButtons(slot, 0) & IN_DUCK.
//   * the original's StartupServer reset (entity-system pointers + clearing the table) is not
//     needed: the port keeps no engine pointers, and every spawn rewrites (or clears) its slot.
//   * the per-frame loop skips slots without a stored speed before the pawn queries, which is
//     exactly when the original's checks could not have written anything either.
//   * Round policy: the write used to happen on every round, including pistol rounds (the
//     reported bug). The optional per-group key "duckspeed_round_min" (vip/groups.ini, read
//     through VIP_GetClientFeatureInt) is seeded at spawn next to the speed value, and the live
//     round (VIP_PistolRound / VIP_GetTotalRounds) is queried once per frame. The threshold is
//     re-read FRESH for every slot that is actually about to be written (alive, ducking, positive
//     speed) right before MovementServices/the write, so a group/config switch (0 -> 1 or 1 -> 0)
//     takes effect without a respawn and a stale cache can never bypass the gate; idle slots do
//     not pay the call. A refused round writes nothing: the engine's ordinary crouch behaviour is
//     untouched and the stored spawn speed is kept, so the effect resumes (same value) on the
//     next eligible round. If the core does not expose the round calls, no write happens (fail
//     closed).

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

// --- PITFALLS B1/B2: the map gate -------------------------------------------------

bool g_mapReady = false;           // a map is up: every client/entity/schema call is allowed
bool g_mapStartSeen = false;       // OnMapStart arrived since OnPluginStart / the last OnMapEnd
bool g_mapEndSeen = false;         // OnMapEnd arrived since OnPluginStart
bool g_mapListenersRegistered = false; // OnMapStart/OnMapEnd are registered with s2sdk

// s2sdk listeners are plain function pointers called on the game thread. They must not throw and
// must not touch a client: OnMapEnd runs while the client list is being torn down (PITFALLS B2).

void OnMapStartListener() {
	// OnMapStart is the map's first live frame; the gate is open for everything that follows.
	// (A plugin loaded into a running map never sees this callback - OnPluginUpdate covers it.)
	g_mapReady = true;
	g_mapStartSeen = true;
	g_mapEndSeen = false;
}

void OnMapEndListener() {
	// From here until the next OnMapStart every s2sdk client query can crash (PITFALLS B2): only
	// state changes, no client call.
	g_mapReady = false;
	g_mapStartSeen = false;
	g_mapEndSeen = true;
}

// --- PITFALLS C8: the schema offsets ------------------------------------------------

// -1 = not resolved yet (or the field is missing): a failed lookup is retried, a successful one
// is kept. Cleared in OnPluginEnd so a reload never reuses an offset of the unloaded library.
int g_movementServicesOffset = -1;
int g_duckSpeedOffset = -1;

// The map-gated replacement of pisex::SchemaOffset (PITFALLS C8): resolving an offset before a
// map exists segfaults inside the schema system, so a closed gate answers -1 and the caller skips
// the work. Only offsets >= 0 are cached.
int SchemaOffset(const char* szClassName, const char* szFieldName, int& iCachedOffset) {
	if (!g_mapReady || __s2sdk_GetSchemaOffset == nullptr) {
		return -1;
	}
	if (iCachedOffset < 0) {
		try {
			iCachedOffset =
				static_cast<int>(s2sdk::GetSchemaOffset(plg::string(szClassName), plg::string(szFieldName)));
		} catch (...) {
			iCachedOffset = -1; // retried on the next frame
		}
	}
	return iCachedOffset;
}

void ResetSchemaOffsets() {
	g_movementServicesOffset = -1;
	g_duckSpeedOffset = -1;
}

// --- PITFALLS C2: the client guards -------------------------------------------------

// A connected slot while a map is up. The controller/pawn checks come on top in IsAlive().
bool IsConnected(int iSlot) {
	if (!g_mapReady || iSlot < 0 || iSlot >= vip_duckspeed::kMaxSlots ||
		__s2sdk_IsClientConnected == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(iSlot);
	} catch (...) {
		return false;
	}
}

// The original's CCSPlayerController::FromSlot + m_hPlayerPawn + IsAlive chain: a connected,
// in-game, non-SourceTV client whose controller entity exists and whose pawn is alive.
bool IsAlive(int iSlot) {
	if (!IsConnected(iSlot) || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr || __s2sdk_PlayerSlotToEntPointer == nullptr ||
		__s2sdk_IsClientAlive == nullptr) {
		return false;
	}
	try {
		if (!s2sdk::IsClientInGame(iSlot) || s2sdk::IsClientSourceTV(iSlot)) {
			return false;
		}
		// The controller entity: without it s2sdk's pawn accessors would warn or read a stale
		// client record (PITFALLS C2).
		if (s2sdk::PlayerSlotToEntPointer(iSlot) == nullptr) {
			return false;
		}
		return s2sdk::IsClientAlive(iSlot);
	} catch (...) {
		return false;
	}
}

// The pawn's movement services (the original's m_pMovementServices), read from the raw pawn
// pointer; the component pointer has no s2sdk helper. Only called for a slot that passed
// IsAlive(), which is what makes the s2sdk::GetClientPawn call warning-free and safe.
void* MovementServices(int iSlot) {
	if (__s2sdk_GetClientPawn == nullptr || !IsConnected(iSlot)) {
		return nullptr;
	}
	const int iOffset =
		SchemaOffset("CBasePlayerPawn", "m_pMovementServices", g_movementServicesOffset);
	if (iOffset < 0) {
		return nullptr;
	}
	try {
		void* pPawn = s2sdk::GetClientPawn(iSlot);
		if (pPawn == nullptr) {
			return nullptr;
		}
		// m_pMovementServices is a pointer field: its value is the component address.
		return *reinterpret_cast<void**>(static_cast<char*>(pPawn) + iOffset);
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
	// DuckSpeedTimer of the original (its callback ran once per frame): now called from
	// OnPluginUpdate while the map gate is open.
	void TickAllPlayers();

	vip_duckspeed::State m_state;
};

DuckSpeedPlugin g_plugin;

void DuckSpeedPlugin::TickAllPlayers() {
	// The original's early-out: no slot with a stored speed (or the s2sdk bindings are missing).
	if (m_state.count <= 0 || __s2sdk_GetClientButtons == nullptr ||
		__s2sdk_GetClientPawn == nullptr || __s2sdk_GetSchemaOffset == nullptr) {
		return;
	}
	// The round API is optional at runtime (an older core): without the core or the two round
	// bindings no write happens at all - a disabled API must never enable the effect.
	if (!__vip_VIP_IsVIPLoaded || !__vip_VIP_PistolRound || !__vip_VIP_GetTotalRounds) {
		return;
	}
	if (!g_pVIPCore->VIP_IsVIPLoaded()) {
		return;
	}
	// The live round, queried once per frame for the whole module (not once per slot). The gate is
	// re-evaluated before each write below, so this is never a cached "allowed" decision.
	const bool bPistolRound = g_pVIPCore->VIP_PistolRound();
	const int iTotalRounds = g_pVIPCore->VIP_GetTotalRounds();

	for (int i = 0; i < vip_duckspeed::kMaxSlots; ++i) {
		// Slots without a positive stored speed are skipped.
		if (m_state.speed[i] <= 0.0f) {
			continue;
		}
		const bool bAlive = IsAlive(i);
		uint64_t nButtons = 0;
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
		// The round threshold is re-read FRESH here, only for a slot that is actually about to be
		// written (alive, ducking, positive speed): the spawn-time cache can be stale after a
		// group/config switch (still 0 while the group now says 1), and a switch the other way
		// (1 -> 0) must also be honoured without a respawn. Idle slots never pay this cross-plugin
		// call. The frame's round state was sampled once above; the cached value is updated from
		// the fresh answer and the live gate runs right before MovementServices/the write. The
		// spawn speed value is not cleared and no engine field is reset.
		int iRoundMin = m_state.roundMin[i]; // replaced by the fresh answer below
		try {
			iRoundMin = g_pVIPCore->VIP_GetClientFeatureInt(i, vip_duckspeed::kRoundMinFeature);
		} catch (...) {
			continue; // no fresh answer -> do not write
		}
		m_state.roundMin[i] = iRoundMin;
		if (!vip_duckspeed::RoundAllowed(iRoundMin, bPistolRound, iTotalRounds)) {
			continue;
		}
		void* pServices = MovementServices(i);
		if (pServices == nullptr) {
			continue;
		}
		// The original's duck-speed write, at the cached offset (the field is a float; the raw
		// write marks no network state, exactly like the original's field-reference write).
		const int iDuckSpeedOffset =
			SchemaOffset("CCSPlayer_MovementServices", "m_flDuckSpeed", g_duckSpeedOffset);
		if (iDuckSpeedOffset < 0) {
			continue;
		}
		try {
			*reinterpret_cast<float*>(static_cast<char*>(pServices) + iDuckSpeedOffset) =
				m_state.speed[i];
		} catch (...) {
		}
	}
}

void DuckSpeedPlugin::OnPlayerSpawn(int iSlot, int /*iTeam*/, bool bIsVIP) {
	// The core only reports real slots; the check keeps a malformed callback from indexing the
	// table (and from reaching the vip API with a slot outside the engine's range).
	if (iSlot < 0 || iSlot >= vip_duckspeed::kMaxSlots) {
		return;
	}
	// The original read the feature for a VIP and used 0 for everyone else. The round threshold is
	// cached at the same moment (the core answers -1 when the group key is missing; RoundAllowed
	// resolves that to the fail-closed fallback of 1).
	const float fDuckSpeed =
		bIsVIP ? g_pVIPCore->VIP_GetClientFeatureFloat(iSlot, kFeature) : 0.0f;
	const int iRoundMin = bIsVIP
							  ? g_pVIPCore->VIP_GetClientFeatureInt(iSlot, vip_duckspeed::kRoundMinFeature)
							  : vip_duckspeed::kDefaultRoundMin;
	vip_duckspeed::OnSpawn(m_state, iSlot, bIsVIP, fDuckSpeed, iRoundMin);
}

plg::PluginResult DuckSpeedPlugin::OnPluginStart() {
	try {
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = false;

		// PITFALLS B1: registering the map listeners touches no client, so it is safe here.
		// Without the listeners nothing could ever close the gate again, so a missing binding
		// keeps it shut for good (fail soft, never Failed - PITFALLS A1).
		if (!g_mapListenersRegistered && __s2sdk_OnMapStart_Register != nullptr &&
			__s2sdk_OnMapEnd_Register != nullptr) {
			s2sdk::OnMapStart_Register(&OnMapStartListener);
			s2sdk::OnMapEnd_Register(&OnMapEndListener);
			g_mapListenersRegistered = true;
		} else if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
			LogTaggedError(
				"the s2sdk map listeners are unavailable - client calls stay disabled");
		}

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
		// PITFALLS B1: a plugin loaded into a running map never saw OnMapStart, so its first
		// update is the "a map is up" signal. After an OnMapEnd only a new OnMapStart re-opens the
		// gate: the host keeps pumping updates through a changelevel.
		if (!g_mapReady && g_mapListenersRegistered && (g_mapStartSeen || !g_mapEndSeen)) {
			g_mapReady = true;
		}
		if (g_mapReady) {
			TickAllPlayers(); // the original's every-frame timer callback
		}
	} catch (const std::exception& ex) {
		LogTaggedError(std::string("OnPluginUpdate failed: ") + ex.what());
	} catch (...) {
		LogTaggedError("OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult DuckSpeedPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7: a plg reload must leave nothing behind that points into this library.
		if (g_mapListenersRegistered) {
			if (__s2sdk_OnMapStart_Unregister != nullptr) {
				s2sdk::OnMapStart_Unregister(&OnMapStartListener);
			}
			if (__s2sdk_OnMapEnd_Unregister != nullptr) {
				s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
			}
			g_mapListenersRegistered = false;
		}
		g_pVIPCore->VIP_OnPlayerSpawnUnregister();

		// No stale schema offset or slot value may survive the unload (PITFALLS C8/A7).
		ResetSchemaOffsets();
		g_mapReady = false;
		g_mapStartSeen = false;
		g_mapEndSeen = true;
		m_state = {};
	} catch (...) {
		// nothing left to do at unload
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_plugin)
