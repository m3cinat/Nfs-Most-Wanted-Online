#pragma once

#include <windows.h>
#include <cstdint>

constexpr std::uintptr_t kSpeedImageBase = 0x00400000u;
constexpr std::uintptr_t kLanSelectCreate = 0x00562A30u;
constexpr std::uintptr_t kLanSelectCtor = 0x00560470u;
constexpr std::uintptr_t kLanSelectParse = 0x00558DF0u;
constexpr std::uintptr_t kLanSelectRefreshState = 0x00558910u;
constexpr std::uintptr_t kLanSelectRedraw = 0x005584D0u;
constexpr std::uintptr_t kLanSelectRebuildRows = 0x0054E560u;
constexpr std::uintptr_t kLanSelectUiEvent = 0x00560640u;
constexpr std::uintptr_t kPcLanEvent = 0x00559090u;
constexpr std::uintptr_t kLanSelectEventEntry = 0x005625C0u;
constexpr std::uintptr_t kLanSelectEventBody = 0x005625CDu;
constexpr std::uintptr_t kLanSelectStartJoin = 0x00560220u;
constexpr std::uintptr_t kLanSelectedHostSetter = 0x0056E480u;
constexpr std::uintptr_t kLanDefaultHostGetter = 0x0056E440u;
constexpr std::uintptr_t kLanDefaultPortGetter = 0x0056E460u;
constexpr std::uintptr_t kLanJoinFlowBuild = 0x005538E0u;
constexpr std::uintptr_t kLanJoinFlowTick = 0x00553980u;
constexpr std::uintptr_t kLanNetConnect = 0x00792BF0u;
constexpr std::uintptr_t kLanNetSessionConnect = 0x00792CA0u;
constexpr std::uintptr_t kLanNetPersonaConnect = 0x00792EF0u;
constexpr std::uintptr_t kLanNetHasHandle = 0x0078B350u;
constexpr std::uintptr_t kLanNetGetState = 0x0078B380u;
constexpr std::uintptr_t kLanNetSyncState = 0x0078B3A0u;
constexpr std::uintptr_t kLanNetReady = 0x0078B420u;
constexpr std::uintptr_t kLanNetErrorEntry = 0x0078B450u;
constexpr std::uintptr_t kLanNetErrorCode = 0x0078B490u;
constexpr std::uintptr_t kLobbyJoinUserset = 0x0079EE50u;
constexpr std::uintptr_t kLobbyJoinName = 0x0079B260u;
constexpr std::uintptr_t kLobbyUjoiCallback = 0x0079AA50u;
constexpr std::uintptr_t kLobbyJoinAutoCallback = 0x0079B3A0u;
constexpr std::uintptr_t kLobbyUsetPlayEvent = 0x0079B520u;
constexpr std::uintptr_t kLanJoinFlowObjectGlobal = 0x0091CA68u;
constexpr std::uintptr_t kLanSelectedHostString = 0x008F42ECu;
constexpr std::uintptr_t kLanLanPortValue = 0x008F430Cu;
constexpr std::uintptr_t kLanOnlinePortValue = 0x008F42E8u;
constexpr std::uintptr_t kOnlineStateGlobal = 0x0091CF90u;
constexpr std::uintptr_t kGameMalloc = 0x00652AD0u;

inline void* SpeedVa(std::uintptr_t va)
{
	HMODULE exe = GetModuleHandleA(nullptr);
	if (!exe)
		return reinterpret_cast<void*>(va);
	return reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(exe) + (va - kSpeedImageBase));
}
