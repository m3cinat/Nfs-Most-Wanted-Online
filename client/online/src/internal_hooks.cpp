#include "internal_hooks.h"

#include "config.h"
#include "detour.h"
#include "lan_select.h"
#include "logger.h"
#include "patterns.h"

#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

using LanCtorFn = void* (__thiscall*)(void*, void*);
using LanParseFn = void (__fastcall*)(void*);
using LanEventFn = void (__thiscall*)(void*, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t);
using LanSetHostFn = void (__stdcall*)(const char*);
using LanRefreshStateFn = std::uint32_t (__fastcall*)(void*);
using LanRebuildRowsFn = void (__fastcall*)(void*);
using LanFastNoArgFn = void (__fastcall*)(void*);
using LanJoinBuildFn = void (__cdecl*)(std::uint32_t);
using LanHostGetterFn = const char* (__cdecl*)();
using LanPortGetterFn = std::uint32_t (__cdecl*)();
using LanNetConnectFn = char (__thiscall*)(void*, const char*, std::uint16_t, std::uint32_t);
using LanNetSessionConnectFn = char (__thiscall*)(void*, const char*, const char*);
using LanNetPersonaConnectFn = char (__thiscall*)(void*, const char*);
using LanNetBoolFn = bool (__fastcall*)(void*);
using LanNetIntFn = std::int32_t (__fastcall*)(void*);
using LanNetVoidFn = void (__fastcall*)(void*);
using LanNetSlotFn = std::uintptr_t (__thiscall*)(void*, int);
using LobbyJoinUsersetFn = int (__thiscall*)(void*, const char*, const char*, void*, void*);
using LobbyUjoiCallbackFn = void (__cdecl*)(std::uint32_t, void*, void*);
using LobbyJoinAutoCallbackFn = void (__fastcall*)(void*);
using LobbyUsetPlayEventFn = void (__cdecl*)(void*, void*, void*);

static InlineDetour gCtorDetour;
static InlineDetour gParseDetour;
static InlineDetour gEventDetour;
static InlineDetour gUiEventDetour;
static InlineDetour gPcLanEventDetour;
static InlineDetour gStartJoinDetour;
static InlineDetour gSetHostDetour;
static InlineDetour gJoinBuildDetour;
static InlineDetour gJoinTickDetour;
static InlineDetour gHostGetterDetour;
static InlineDetour gPortGetterDetour;
static InlineDetour gNetConnectDetour;
static InlineDetour gNetSessionConnectDetour;
static InlineDetour gNetPersonaConnectDetour;
static InlineDetour gNetHasHandleDetour;
static InlineDetour gNetGetStateDetour;
static InlineDetour gNetSyncStateDetour;
static InlineDetour gNetReadyDetour;
static InlineDetour gNetErrorEntryDetour;
static InlineDetour gNetErrorCodeDetour;
static InlineDetour gLobbyJoinUsersetDetour;
static InlineDetour gLobbyJoinNameDetour;
static InlineDetour gLobbyUjoiCallbackDetour;
static InlineDetour gLobbyJoinAutoCallbackDetour;
static InlineDetour gLobbyUsetPlayEventDetour;
static bool gInParseHook = false;
static volatile LONG gPcLanPollCounter = 0;
static volatile LONG gJoinTickCounter = 0;
static volatile LONG gNetStatusCounter = 0;
static volatile LONG gLastLanNetThis = 0;

static LanCtorFn OrigCtor()
{
	return reinterpret_cast<LanCtorFn>(gCtorDetour.trampoline);
}

static LanParseFn OrigParse()
{
	return reinterpret_cast<LanParseFn>(gParseDetour.trampoline);
}

static LanEventFn OrigEvent()
{
	return reinterpret_cast<LanEventFn>(gEventDetour.trampoline);
}

static LanEventFn OrigUiEvent()
{
	return reinterpret_cast<LanEventFn>(gUiEventDetour.trampoline);
}

static LanEventFn OrigPcLanEvent()
{
	return reinterpret_cast<LanEventFn>(gPcLanEventDetour.trampoline);
}

static LanSetHostFn OrigSetHost()
{
	return reinterpret_cast<LanSetHostFn>(gSetHostDetour.trampoline);
}

static LanFastNoArgFn OrigStartJoin()
{
	return reinterpret_cast<LanFastNoArgFn>(gStartJoinDetour.trampoline);
}

static LanJoinBuildFn OrigJoinBuild()
{
	return reinterpret_cast<LanJoinBuildFn>(gJoinBuildDetour.trampoline);
}

static LanFastNoArgFn OrigJoinTick()
{
	return reinterpret_cast<LanFastNoArgFn>(gJoinTickDetour.trampoline);
}

static LanHostGetterFn OrigHostGetter()
{
	return reinterpret_cast<LanHostGetterFn>(gHostGetterDetour.trampoline);
}

static LanPortGetterFn OrigPortGetter()
{
	return reinterpret_cast<LanPortGetterFn>(gPortGetterDetour.trampoline);
}

static LanNetConnectFn OrigNetConnect()
{
	return reinterpret_cast<LanNetConnectFn>(gNetConnectDetour.trampoline);
}

static LanNetSessionConnectFn OrigNetSessionConnect()
{
	return reinterpret_cast<LanNetSessionConnectFn>(gNetSessionConnectDetour.trampoline);
}

static LanNetPersonaConnectFn OrigNetPersonaConnect()
{
	return reinterpret_cast<LanNetPersonaConnectFn>(gNetPersonaConnectDetour.trampoline);
}

static LanNetBoolFn OrigNetHasHandle()
{
	return reinterpret_cast<LanNetBoolFn>(gNetHasHandleDetour.trampoline);
}

static LanNetIntFn OrigNetGetState()
{
	return reinterpret_cast<LanNetIntFn>(gNetGetStateDetour.trampoline);
}

static LanNetVoidFn OrigNetSyncState()
{
	return reinterpret_cast<LanNetVoidFn>(gNetSyncStateDetour.trampoline);
}

static LanNetBoolFn OrigNetReady()
{
	return reinterpret_cast<LanNetBoolFn>(gNetReadyDetour.trampoline);
}

static LanNetSlotFn OrigNetErrorEntry()
{
	return reinterpret_cast<LanNetSlotFn>(gNetErrorEntryDetour.trampoline);
}

static LanNetSlotFn OrigNetErrorCode()
{
	return reinterpret_cast<LanNetSlotFn>(gNetErrorCodeDetour.trampoline);
}

static LobbyJoinUsersetFn OrigLobbyJoinUserset()
{
	return reinterpret_cast<LobbyJoinUsersetFn>(gLobbyJoinUsersetDetour.trampoline);
}

static LobbyJoinUsersetFn OrigLobbyJoinName()
{
	return reinterpret_cast<LobbyJoinUsersetFn>(gLobbyJoinNameDetour.trampoline);
}

static LobbyUjoiCallbackFn OrigLobbyUjoiCallback()
{
	return reinterpret_cast<LobbyUjoiCallbackFn>(gLobbyUjoiCallbackDetour.trampoline);
}

static LobbyJoinAutoCallbackFn OrigLobbyJoinAutoCallback()
{
	return reinterpret_cast<LobbyJoinAutoCallbackFn>(gLobbyJoinAutoCallbackDetour.trampoline);
}

static LobbyUsetPlayEventFn OrigLobbyUsetPlayEvent()
{
	return reinterpret_cast<LobbyUsetPlayEventFn>(gLobbyUsetPlayEventDetour.trampoline);
}

static std::uint8_t* LanOpenSelectedFlag()
{
	auto stateSlot = reinterpret_cast<std::uintptr_t*>(SpeedVa(kOnlineStateGlobal));
	if (!stateSlot || *stateSlot == 0)
		return nullptr;
	return reinterpret_cast<std::uint8_t*>(*stateSlot + 0x172);
}

static std::uintptr_t OnlineStateBase()
{
	auto stateSlot = reinterpret_cast<std::uintptr_t*>(SpeedVa(kOnlineStateGlobal));
	if (!stateSlot)
		return 0;
	return *stateSlot;
}

static int ReadLanOpenSelectedFlag()
{
	std::uint8_t* flag = LanOpenSelectedFlag();
	return flag ? (int)*flag : -1;
}

static bool IsLanOpenFlagEvent(std::uint32_t hash)
{
	return hash == 0x0c407210u || hash == 0x406415e3u ||
		hash == 0xc519bfc0u || hash == 0x911ab364u ||
		hash == 0x6b7baa6fu;
}

static void LogLanOpenSelectedFlag(const char* tag, std::uint32_t hash)
{
	std::uint8_t* flag = LanOpenSelectedFlag();
	LogLine(
		"%s open-selected-flag hash=0x%08lX name=%s ptr=%p value=%d",
		tag ? tag : "lan-select",
		(unsigned long)hash,
		LanSelectEventName(hash),
		flag,
		flag ? (int)*flag : -1);
}

static const char* PcLanEventName(std::uint32_t hash)
{
	switch (hash) {
	case 0xc98356ba: return "pc-lan-poll";
	case 0xda5b8712: return "pc-lan-advance";
	case 0xc9d30688: return "pc-lan-cancel";
	case 0x911ab364: return "ui-confirm";
	default: return LanSelectEventName(hash);
	}
}

static void LogPcLanEvent(const char* tag, void* self, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4)
{
	std::uintptr_t state = OnlineStateBase();
	LogLine(
		"%s pc-lan-event this=%p hash=0x%08lX name=%s arg2=0x%08lX arg3=0x%08lX arg4=0x%08lX page10=0x%08lX req30=0x%08lX timer34=0x%08lX busy38=%u open_flag=%d online_state=0x%08lX",
		tag ? tag : "lan-select",
		self,
		(unsigned long)hash,
		PcLanEventName(hash),
		(unsigned long)arg2,
		(unsigned long)arg3,
		(unsigned long)arg4,
		self ? (unsigned long)*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + 0x10) : 0,
		self ? (unsigned long)*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + 0x30) : 0,
		self ? (unsigned long)*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + 0x34) : 0,
		self ? (unsigned)*reinterpret_cast<std::uint8_t*>(reinterpret_cast<std::uint8_t*>(self) + 0x38) : 0,
		ReadLanOpenSelectedFlag(),
		(unsigned long)state);
}

static void CopyStringPreview(char* out, std::size_t outSize, const char* src, std::size_t maxLen)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	if (!src)
		return;
	for (std::size_t i = 0; i + 1 < outSize && i < maxLen; ++i) {
		unsigned char c = (unsigned char)src[i];
		if (c == 0)
			break;
		out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
		out[i + 1] = 0;
	}
}

static bool CanReadAddress(std::uintptr_t addr, std::size_t size)
{
	if (!addr || size == 0)
		return false;
	MEMORY_BASIC_INFORMATION mbi{};
	if (VirtualQuery(reinterpret_cast<const void*>(addr), &mbi, sizeof(mbi)) == 0)
		return false;
	if (mbi.State != MEM_COMMIT)
		return false;
	if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0)
		return false;
	std::uintptr_t base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
	std::uintptr_t end = addr + size;
	std::uintptr_t regionEnd = base + mbi.RegionSize;
	return end >= addr && addr >= base && end <= regionEnd;
}

static std::uintptr_t ReadGlobalPtr(std::uintptr_t va)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(SpeedVa(va));
	if (!CanReadAddress(addr, sizeof(std::uintptr_t)))
		return 0;
	return *reinterpret_cast<std::uintptr_t*>(addr);
}

static std::uint32_t ReadU32(std::uintptr_t base, std::size_t offset)
{
	if (!base)
		return 0;
	if (!CanReadAddress(base + offset, sizeof(std::uint32_t)))
		return 0;
	return *reinterpret_cast<std::uint32_t*>(base + offset);
}

static std::uint8_t ReadU8(std::uintptr_t base, std::size_t offset)
{
	if (!base)
		return 0;
	if (!CanReadAddress(base + offset, sizeof(std::uint8_t)))
		return 0;
	return *reinterpret_cast<std::uint8_t*>(base + offset);
}

static void FourCCPreview(char* out, std::size_t outSize, std::uint32_t value)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	if (outSize < 5)
		return;
	for (int i = 0; i < 4; ++i) {
		unsigned char c = (unsigned char)((value >> (i * 8)) & 0xff);
		out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
	}
	out[4] = 0;
}

static std::uint16_t ReadGlobalU16(std::uintptr_t va)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(SpeedVa(va));
	if (!CanReadAddress(addr, sizeof(std::uint16_t)))
		return 0;
	return *reinterpret_cast<std::uint16_t*>(addr);
}

static void LogJoinFlowState(const char* tag, void* self)
{
	std::uintptr_t state = OnlineStateBase();
	std::uintptr_t obj = ReadGlobalPtr(kLanJoinFlowObjectGlobal);
	std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);
	char selectedHost[80] = {};
	CopyStringPreview(selectedHost, sizeof(selectedHost), reinterpret_cast<const char*>(SpeedVa(kLanSelectedHostString)), 64);
	LogLine(
		"%s join-flow this=%p obj=0x%08lX state=0x%08lX flags12c=0x%08lX mode19c=%ld open_flag=%d host='%s' lan_port=%u online_port=%u self+04=0x%08lX self+08=%u self+09=%u self+0c=%u self+0e=%u self+10=%u self+11=%u self+2c=0x%08lX self+3c=%u obj+04=0x%08lX obj+08=%u obj+09=%u obj+11=%u",
		tag ? tag : "lan-select",
		self,
		(unsigned long)obj,
		(unsigned long)state,
		(unsigned long)ReadU32(state, 0x12c),
		(long)ReadU32(state, 0x19c),
		ReadLanOpenSelectedFlag(),
		selectedHost[0] ? selectedHost : "-",
		(unsigned)ReadGlobalU16(kLanLanPortValue),
		(unsigned)ReadGlobalU16(kLanOnlinePortValue),
		(unsigned long)ReadU32(selfAddr, 0x04),
		(unsigned)ReadU8(selfAddr, 0x08),
		(unsigned)ReadU8(selfAddr, 0x09),
		(unsigned)ReadU8(selfAddr, 0x0c),
		(unsigned)ReadU8(selfAddr, 0x0e),
		(unsigned)ReadU8(selfAddr, 0x10),
		(unsigned)ReadU8(selfAddr, 0x11),
		(unsigned long)ReadU32(selfAddr, 0x2c),
		(unsigned)ReadU8(selfAddr, 0x3c),
		(unsigned long)ReadU32(obj, 0x04),
		(unsigned)ReadU8(obj, 0x08),
		(unsigned)ReadU8(obj, 0x09),
		(unsigned)ReadU8(obj, 0x11));
}

static bool ShouldLogNetStatus(void* self, bool force)
{
	if (force)
		return true;
	std::uintptr_t last = (std::uintptr_t)InterlockedCompareExchange(&gLastLanNetThis, 0, 0);
	if (last != 0 && reinterpret_cast<std::uintptr_t>(self) == last)
		return true;
	LONG n = InterlockedIncrement(&gNetStatusCounter);
	return n <= 16 || (n % 200) == 0;
}

static void LogNetObjectState(const char* tag, void* self)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(self);
	std::uintptr_t handle = ReadU32(addr, 0x00);
	std::uint32_t idx = ReadU32(handle, 0x58);
	std::uint32_t curState = (idx <= 0x20) ? ReadU32(handle, 0x18 + (idx * 4)) : 0xffffffffu;
	std::uint32_t readyCode = (handle && curState == ReadU32(handle, 0x18 + (idx * 4))) ? ReadU32(handle, 0x5c) : 3;
	char h188[8] = {};
	char h18c[8] = {};
	FourCCPreview(h188, sizeof(h188), ReadU32(handle, 0x188));
	FourCCPreview(h18c, sizeof(h18c), ReadU32(handle, 0x18c));
	LogLine(
		"%s net-state this=%p handle=0x%08lX last_net=0x%08lX h.idx=%lu h.cur=%ld h.ready=%ld h.saved=0x%08lX h.pending=0x%08lX h.188=0x%08lX('%s') h.18c=0x%08lX('%s') dword04=0x%08lX dword88=0x%08lX dwordd0=0x%08lX dword130=0x%08lX dword144=0x%08lX",
		tag ? tag : "lan-select",
		self,
		(unsigned long)handle,
		(unsigned long)InterlockedCompareExchange(&gLastLanNetThis, 0, 0),
		(unsigned long)idx,
		(long)curState,
		(long)readyCode,
		(unsigned long)ReadU32(handle, 0x5c),
		(unsigned long)ReadU32(handle, 0x60),
		(unsigned long)ReadU32(handle, 0x188),
		h188,
		(unsigned long)ReadU32(handle, 0x18c),
		h18c,
		(unsigned long)ReadU32(addr, 0x04),
		(unsigned long)ReadU32(addr, 0x88),
		(unsigned long)ReadU32(addr, 0xd0),
		(unsigned long)ReadU32(addr, 0x130),
		(unsigned long)ReadU32(addr, 0x144));
}

static void LogLobbyJoinObjectState(const char* tag, void* self)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(self);
	char pending188[8] = {};
	char pending18c[8] = {};
	FourCCPreview(pending188, sizeof(pending188), ReadU32(addr, 0x188));
	FourCCPreview(pending18c, sizeof(pending18c), ReadU32(addr, 0x18c));
	LogLine(
		"%s this=%p game1c=%ld dword10=0x%08lX dword14=0x%08lX dword18=0x%08lX dword100=0x%08lX dword104=0x%08lX byte10c=%u byte10d=%u byte10e=%u byte10f=%u pending188=0x%08lX('%s') pending18c=0x%08lX('%s')",
		tag ? tag : "lobby-join",
		self,
		(long)ReadU32(addr, 0x1c),
		(unsigned long)ReadU32(addr, 0x10),
		(unsigned long)ReadU32(addr, 0x14),
		(unsigned long)ReadU32(addr, 0x18),
		(unsigned long)ReadU32(addr, 0x100),
		(unsigned long)ReadU32(addr, 0x104),
		(unsigned)ReadU8(addr, 0x10c),
		(unsigned)ReadU8(addr, 0x10d),
		(unsigned)ReadU8(addr, 0x10e),
		(unsigned)ReadU8(addr, 0x10f),
		(unsigned long)ReadU32(addr, 0x188),
		pending188,
		(unsigned long)ReadU32(addr, 0x18c),
		pending18c);
}

static void LogLobbyCallbackPacket(const char* tag, void* packet)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(packet);
	char name44[96] = {};
	char payloadAc[160] = {};
	CopyStringPreview(name44, sizeof(name44), reinterpret_cast<const char*>(addr + 0x44), 80);
	CopyStringPreview(payloadAc, sizeof(payloadAc), reinterpret_cast<const char*>(addr + 0xac), 140);
	LogLine(
		"%s packet=%p dword08=0x%08lX dword0c=0x%08lX dword20=0x%08lX flags28=0x%08lX count214=%ld name44='%s' payloadAc='%s'",
		tag ? tag : "lobby-callback",
		packet,
		(unsigned long)ReadU32(addr, 0x08),
		(unsigned long)ReadU32(addr, 0x0c),
		(unsigned long)ReadU32(addr, 0x20),
		(unsigned long)ReadU32(addr, 0x28),
		(long)ReadU32(addr, 0x214),
		name44[0] ? name44 : "-",
		payloadAc[0] ? payloadAc : "-");
}

static void LogLobbyEventCandidate(const char* tag, void* candidate)
{
	std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(candidate);
	std::uintptr_t payload = ReadU32(addr, 0x10);
	char fourcc[8] = {};
	char payloadPreview[180] = {};
	FourCCPreview(fourcc, sizeof(fourcc), ReadU32(addr, 0x08));
	if (CanReadAddress(payload, 160))
		CopyStringPreview(payloadPreview, sizeof(payloadPreview), reinterpret_cast<const char*>(payload), 160);
	LogLine(
		"%s ptr=%p dword08=0x%08lX('%s') dword0c=0x%08lX payload_ptr=0x%08lX payload='%s'",
		tag ? tag : "lobby-event-candidate",
		candidate,
		(unsigned long)ReadU32(addr, 0x08),
		fourcc[0] ? fourcc : "-",
		(unsigned long)ReadU32(addr, 0x0c),
		(unsigned long)payload,
		payloadPreview[0] ? payloadPreview : "-");
}

static void CallRebuildAndRefresh(void* self)
{
	auto rebuildRows = reinterpret_cast<LanRebuildRowsFn>(SpeedVa(kLanSelectRebuildRows));
	LONG refreshCfg = InterlockedCompareExchange(&gLanInternalRefreshStateEnabled, 0, 0);
	LogLine(
		"lan-select fake refresh call rebuild=0x%08lX refresh_state=0x%08lX config_refresh_state=%ld forced=0",
		(unsigned long)kLanSelectRebuildRows,
		(unsigned long)kLanSelectRefreshState,
		(long)refreshCfg);
	rebuildRows(self);
	if (refreshCfg != 0) {
		auto refreshState = reinterpret_cast<LanRefreshStateFn>(SpeedVa(kLanSelectRefreshState));
		refreshState(self);
	}
}

static void* __fastcall HookLanCtor(void* self, void*, void* param1)
{
	LogLine("lan-select ctor enter addr=0x%08lX this=%p param1=%p", (unsigned long)kLanSelectCtor, self, param1);
	DumpLanSelectCore("lan-select ctor before", self);
	void* ret = OrigCtor()(self, param1);
	LogLine("lan-select ctor leave this=%p ret=%p", self, ret);
	DumpLanSelectCore("lan-select ctor after", self);
	DumpLanServerList("lan-select ctor after", self);
	return ret;
}

static void __fastcall HookLanParse(void* self)
{
	if (gInParseHook) {
		OrigParse()(self);
		return;
	}

	gInParseHook = true;
	LogLine("lan-select parse enter addr=0x%08lX this=%p", (unsigned long)kLanSelectParse, self);
	DumpLanSelectCore("lan-select parse before", self);
	DumpLanServerList("lan-select parse before", self);
	OrigParse()(self);
	LogLine("lan-select parse original returned this=%p", self);
	DumpLanSelectCore("lan-select parse after-original", self);
	DumpLanServerList("lan-select parse after-original", self);
	if (InjectFakeLanServerEntry(self)) {
		DumpLanServerList("lan-select parse after-fake", self);
		CallRebuildAndRefresh(self);
		DumpLanSelectCore("lan-select parse after-refresh", self);
		DumpLanServerList("lan-select parse after-refresh", self);
	}
	gInParseHook = false;
}

static void __fastcall HookLanEvent(void* self, void*, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4)
{
	LogLanSelectEvent("lan-select main-event enter", self, hash, arg2, arg3, arg4);
	DumpLanSelectCore("lan-select event before", self);
	OrigEvent()(self, hash, arg2, arg3, arg4);
	LogLanSelectEvent("lan-select main-event leave", self, hash, arg2, arg3, arg4);
	DumpLanSelectCore("lan-select event after", self);
}

static void __fastcall HookLanUiEvent(void* self, void*, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4)
{
	if (hash == 0xc98356bau || hash == 0x9803f6e2u) {
		OrigUiEvent()(self, hash, arg2, arg3, arg4);
		return;
	}

	if (IsLanOpenFlagEvent(hash))
		LogLanOpenSelectedFlag("lan-select ui-event before", hash);
	LogLanSelectEvent("lan-select ui-event enter", self, hash, arg2, arg3, arg4);
	OrigUiEvent()(self, hash, arg2, arg3, arg4);
	LogLanSelectEvent("lan-select ui-event leave", self, hash, arg2, arg3, arg4);
	if (IsLanOpenFlagEvent(hash))
		LogLanOpenSelectedFlag("lan-select ui-event after", hash);
}

static void __fastcall HookPcLanEvent(void* self, void*, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4)
{
	bool logEvent = hash != 0xc98356bau;
	if (!logEvent) {
		LONG n = InterlockedIncrement(&gPcLanPollCounter);
		logEvent = (n == 1 || (n % 120) == 0);
	}
	if (logEvent)
		LogPcLanEvent("lan-select pc-lan enter", self, hash, arg2, arg3, arg4);
	OrigPcLanEvent()(self, hash, arg2, arg3, arg4);
	if (logEvent)
		LogPcLanEvent("lan-select pc-lan leave", self, hash, arg2, arg3, arg4);
}

static void __fastcall HookLanStartJoin(void* self, void*)
{
	LogLanSelectJoinState("lan-select start-join enter", self);
	OrigStartJoin()(self);
	LogLanSelectJoinState("lan-select start-join leave", self);
}

static void __cdecl HookLanJoinBuild(std::uint32_t page)
{
	LogJoinFlowState("lan-select join-build enter", nullptr);
	LogLine("lan-select join-build page=0x%08lX addr=0x%08lX", (unsigned long)page, (unsigned long)kLanJoinFlowBuild);
	OrigJoinBuild()(page);
	LogJoinFlowState("lan-select join-build leave", reinterpret_cast<void*>(ReadGlobalPtr(kLanJoinFlowObjectGlobal)));
}

static void __fastcall HookLanJoinTick(void* self, void*)
{
	LONG n = InterlockedIncrement(&gJoinTickCounter);
	std::uintptr_t selfAddr = reinterpret_cast<std::uintptr_t>(self);
	std::uintptr_t objAddr = ReadGlobalPtr(kLanJoinFlowObjectGlobal);
	std::uint32_t stageBefore = ReadU32(selfAddr, 0x04);
	std::uint32_t objStageBefore = ReadU32(objAddr, 0x04);
	bool logBefore = n <= 8 ||
		(n % 120) == 0 ||
		stageBefore == 1 || stageBefore == 2 || stageBefore == 3 ||
		objStageBefore == 1 || objStageBefore == 2 || objStageBefore == 3 ||
		objStageBefore == 5 || objStageBefore == 6;
	if (logBefore) {
		LogJoinFlowState("lan-select join-tick enter", self);
		LogNetObjectState("lan-select join-tick enter", reinterpret_cast<void*>((std::uintptr_t)InterlockedCompareExchange(&gLastLanNetThis, 0, 0)));
	}
	OrigJoinTick()(self);
	std::uint32_t stageAfter = ReadU32(selfAddr, 0x04);
	std::uint32_t objStageAfter = ReadU32(objAddr, 0x04);
	if (logBefore || stageAfter != stageBefore || objStageAfter != objStageBefore) {
		LogJoinFlowState("lan-select join-tick leave", self);
		LogNetObjectState("lan-select join-tick leave", reinterpret_cast<void*>((std::uintptr_t)InterlockedCompareExchange(&gLastLanNetThis, 0, 0)));
	}
}

static const char* __cdecl HookLanDefaultHost()
{
	const char* ret = OrigHostGetter()();
	const char* effective = ret;
	char host[80] = {};
	CopyStringPreview(host, sizeof(host), ret, 64);
	bool onlineDefault = ret && (ret[0] == '*' || std::strstr(ret, "pcnfs06.ea.com") != nullptr);
	if (onlineDefault && InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0) != 0 && gOnline.host[0])
		effective = gOnline.host;
	char effectiveHost[80] = {};
	CopyStringPreview(effectiveHost, sizeof(effectiveHost), effective, 64);
	LogLine(
		"lan-select host-getter ret='%s' effective='%s' ptr=%p online_override=%ld flags12c=0x%08lX open_flag=%d",
		host[0] ? host : "-",
		effectiveHost[0] ? effectiveHost : "-",
		ret,
		(long)InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0),
		(unsigned long)ReadU32(OnlineStateBase(), 0x12c),
		ReadLanOpenSelectedFlag());
	return effective;
}

static std::uint32_t __cdecl HookLanDefaultPort()
{
	std::uint32_t ret = OrigPortGetter()();
	std::uint32_t effective = ret;
	std::uint16_t port = (std::uint16_t)(ret & 0xffffu);
	if (port == 30920 && InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0) != 0 && gOnline.port != 0)
		effective = (ret & 0xffff0000u) | gOnline.port;
	else if (InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0) != 0 && gLan.port != 0)
		effective = (ret & 0xffff0000u) | gLan.port;
	LogLine(
		"lan-select port-getter ret_raw=0x%08lX port=%u effective_raw=0x%08lX effective_port=%u online_override=%ld lan_override=%ld flags12c=0x%08lX open_flag=%d",
		(unsigned long)ret,
		(unsigned)port,
		(unsigned long)effective,
		(unsigned)(effective & 0xffffu),
		(long)InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0),
		(unsigned long)ReadU32(OnlineStateBase(), 0x12c),
		ReadLanOpenSelectedFlag());
	return effective;
}

static char __fastcall HookLanNetConnect(void* self, void*, const char* host, std::uint16_t port, std::uint32_t timeout)
{
	InterlockedExchange(&gLastLanNetThis, (LONG)reinterpret_cast<std::uintptr_t>(self));
	char hostPreview[80] = {};
	CopyStringPreview(hostPreview, sizeof(hostPreview), host, 64);
	LogNetObjectState("lan-select net-connect before", self);
	LogLine(
		"lan-select net-connect enter this=%p host='%s' port=%u timeout=%lu open_flag=%d",
		self,
		hostPreview[0] ? hostPreview : "-",
		(unsigned)port,
		(unsigned long)timeout,
		ReadLanOpenSelectedFlag());
	char ret = OrigNetConnect()(self, host, port, timeout);
	LogLine(
		"lan-select net-connect leave this=%p ret=%d host='%s' port=%u",
		self,
		(int)ret,
		hostPreview[0] ? hostPreview : "-",
		(unsigned)port);
	LogNetObjectState("lan-select net-connect after", self);
	return ret;
}

static char __fastcall HookLanNetSessionConnect(void* self, void*, const char* host, const char* extra)
{
	char hostPreview[80] = {};
	char extraPreview[80] = {};
	CopyStringPreview(hostPreview, sizeof(hostPreview), host, 64);
	CopyStringPreview(extraPreview, sizeof(extraPreview), extra, 64);
	LogLine("lan-select net-session enter this=%p host='%s' extra='%s'", self, hostPreview[0] ? hostPreview : "-", extraPreview[0] ? extraPreview : "-");
	LogNetObjectState("lan-select net-session before", self);
	char ret = OrigNetSessionConnect()(self, host, extra);
	LogLine("lan-select net-session leave this=%p ret=%d", self, (int)ret);
	LogNetObjectState("lan-select net-session after", self);
	return ret;
}

static char __fastcall HookLanNetPersonaConnect(void* self, void*, const char* value)
{
	char preview[80] = {};
	CopyStringPreview(preview, sizeof(preview), value, 64);
	LogLine("lan-select net-persona enter this=%p value='%s'", self, preview[0] ? preview : "-");
	LogNetObjectState("lan-select net-persona before", self);
	char ret = OrigNetPersonaConnect()(self, value);
	LogLine("lan-select net-persona leave this=%p ret=%d", self, (int)ret);
	LogNetObjectState("lan-select net-persona after", self);
	return ret;
}

static bool __fastcall HookLanNetHasHandle(void* self)
{
	bool ret = OrigNetHasHandle()(self);
	if (ShouldLogNetStatus(self, false))
		LogLine("lan-select net-has-handle this=%p ret=%d handle=0x%08lX", self, (int)ret, (unsigned long)ReadU32(reinterpret_cast<std::uintptr_t>(self), 0));
	return ret;
}

static std::int32_t __fastcall HookLanNetGetState(void* self)
{
	std::int32_t ret = OrigNetGetState()(self);
	if (ShouldLogNetStatus(self, true))
		LogLine("lan-select net-get-state this=%p ret=%ld handle=0x%08lX", self, (long)ret, (unsigned long)ReadU32(reinterpret_cast<std::uintptr_t>(self), 0));
	return ret;
}

static void __fastcall HookLanNetSyncState(void* self)
{
	if (ShouldLogNetStatus(self, true))
		LogLine("lan-select net-sync-state enter this=%p handle=0x%08lX", self, (unsigned long)ReadU32(reinterpret_cast<std::uintptr_t>(self), 0));
	OrigNetSyncState()(self);
	if (ShouldLogNetStatus(self, true))
		LogLine("lan-select net-sync-state leave this=%p handle=0x%08lX", self, (unsigned long)ReadU32(reinterpret_cast<std::uintptr_t>(self), 0));
}

static bool __fastcall HookLanNetReady(void* self)
{
	bool ret = OrigNetReady()(self);
	if (ShouldLogNetStatus(self, true))
		LogLine("lan-select net-ready this=%p ret=%d handle=0x%08lX", self, (int)ret, (unsigned long)ReadU32(reinterpret_cast<std::uintptr_t>(self), 0));
	return ret;
}

static std::uintptr_t __fastcall HookLanNetErrorEntry(void* self, void*, int slot)
{
	std::uintptr_t ret = OrigNetErrorEntry()(self, slot);
	LogLine("lan-select net-error-entry this=%p slot=%d ret=0x%08lX", self, slot, (unsigned long)ret);
	return ret;
}

static std::uintptr_t __fastcall HookLanNetErrorCode(void* self, void*, int slot)
{
	std::uintptr_t ret = OrigNetErrorCode()(self, slot);
	LogLine("lan-select net-error-code this=%p slot=%d ret=0x%08lX", self, slot, (unsigned long)ret);
	return ret;
}

static int __fastcall HookLobbyJoinUserset(void* self, void*, const char* userset, const char* pass, void* callback, void* context)
{
	char usersetPreview[96] = {};
	char passPreview[48] = {};
	CopyStringPreview(usersetPreview, sizeof(usersetPreview), userset, 80);
	CopyStringPreview(passPreview, sizeof(passPreview), pass, 32);
	LogLine(
		"lobby-join-userset enter addr=0x%08lX this=%p userset='%s' pass='%s' callback=%p context=%p",
		(unsigned long)kLobbyJoinUserset,
		self,
		usersetPreview[0] ? usersetPreview : "-",
		passPreview[0] ? passPreview : "-",
		callback,
		context);
	LogLobbyJoinObjectState("lobby-join-userset before", self);
	int ret = OrigLobbyJoinUserset()(self, userset, pass, callback, context);
	LogLine("lobby-join-userset leave this=%p ret=%d userset='%s'", self, ret, usersetPreview[0] ? usersetPreview : "-");
	LogLobbyJoinObjectState("lobby-join-userset after", self);
	return ret;
}

static int __fastcall HookLobbyJoinName(void* self, void*, const char* name, const char* pass, void* callback, void* context)
{
	char namePreview[96] = {};
	char passPreview[48] = {};
	CopyStringPreview(namePreview, sizeof(namePreview), name, 80);
	CopyStringPreview(passPreview, sizeof(passPreview), pass, 32);
	LogLine(
		"lobby-join-name enter addr=0x%08lX this=%p name='%s' pass='%s' callback=%p context=%p",
		(unsigned long)kLobbyJoinName,
		self,
		namePreview[0] ? namePreview : "-",
		passPreview[0] ? passPreview : "-",
		callback,
		context);
	LogLobbyJoinObjectState("lobby-join-name before", self);
	int ret = OrigLobbyJoinName()(self, name, pass, callback, context);
	LogLine("lobby-join-name leave this=%p ret=%d name='%s'", self, ret, namePreview[0] ? namePreview : "-");
	LogLobbyJoinObjectState("lobby-join-name after", self);
	return ret;
}

static void __cdecl HookLobbyUjoiCallback(std::uint32_t param1, void* request, void* packet)
{
	LogLine("lobby-ujoi-callback enter addr=0x%08lX param1=0x%08lX request=%p packet=%p", (unsigned long)kLobbyUjoiCallback, (unsigned long)param1, request, packet);
	LogLobbyCallbackPacket("lobby-ujoi-callback packet", packet);
	OrigLobbyUjoiCallback()(param1, request, packet);
	LogLine("lobby-ujoi-callback leave request=%p packet=%p", request, packet);
}

static void __fastcall HookLobbyJoinAutoCallback(void* packet)
{
	LogLine("lobby-join-auto-callback enter addr=0x%08lX packet=%p", (unsigned long)kLobbyJoinAutoCallback, packet);
	LogLobbyCallbackPacket("lobby-join-auto-callback packet", packet);
	OrigLobbyJoinAutoCallback()(packet);
	LogLine("lobby-join-auto-callback leave packet=%p", packet);
}

static void __cdecl HookLobbyUsetPlayEvent(void* arg1, void* eventPacket, void* joinObject)
{
	LogLine(
		"lobby-uset-play-event enter addr=0x%08lX arg1=%p event=%p join_object=%p mode13c=%ld mode19c=%ld",
		(unsigned long)kLobbyUsetPlayEvent,
		arg1,
		eventPacket,
		joinObject,
		(long)ReadU32(OnlineStateBase(), 0x13c),
		(long)ReadU32(OnlineStateBase(), 0x19c));
	LogLobbyEventCandidate("lobby-uset-play-event arg1", arg1);
	LogLobbyEventCandidate("lobby-uset-play-event event", eventPacket);
	LogLobbyJoinObjectState("lobby-uset-play-event join-object before", joinObject);
	OrigLobbyUsetPlayEvent()(arg1, eventPacket, joinObject);
	LogLobbyJoinObjectState("lobby-uset-play-event join-object after", joinObject);
	LogLine("lobby-uset-play-event leave event=%p join_object=%p", eventPacket, joinObject);
}

static void __stdcall HookLanSetHost(const char* host)
{
	char requested[80] = {};
	char effective[80] = {};
	CopyStringPreview(requested, sizeof(requested), host, 64);
	const char* selected = host;
	if (InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0) != 0 && gLan.host[0])
		selected = gLan.host;
	CopyStringPreview(effective, sizeof(effective), selected, 64);
	int flagBefore = ReadLanOpenSelectedFlag();
	LogLine(
		"lan-select selected-host set requested='%s' effective='%s' override=%ld open_selected_before=%d",
		requested[0] ? requested : "-",
		effective[0] ? effective : "-",
		(long)InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0),
		flagBefore);
	OrigSetHost()(selected);
	std::uint8_t* flag = LanOpenSelectedFlag();
	LONG forceOpen = InterlockedCompareExchange(&gLanInternalForceOpenSelectedEnabled, 0, 0);
	LONG fakeEnabled = InterlockedCompareExchange(&gLanInternalFakeEnabled, 0, 0);
	int afterOriginal = flag ? (int)*flag : -1;
	if (forceOpen != 0 && fakeEnabled != 0) {
		LogLine(
			"lan-select selected-host force open-selected ignored unsafe ptr=%p after_original=%d",
			flag,
			afterOriginal);
	} else {
		LogLine(
			"lan-select selected-host flag after_original=%d force_open_selected=%ld fake=%ld",
			afterOriginal,
			(long)forceOpen,
			(long)fakeEnabled);
	}
}

void InitInternalHooks()
{
	LONG internalCfg = InterlockedCompareExchange(&gLanInternalHooksEnabled, 0, 0);
	LONG onlineOverrideCfg = InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0);
	if (internalCfg == 0) {
		if (onlineOverrideCfg != 0) {
			InstallInlineDetour(&gHostGetterDetour, SpeedVa(kLanDefaultHostGetter), reinterpret_cast<void*>(&HookLanDefaultHost), 5, "default host getter 0x0056E440");
			InstallInlineDetour(&gPortGetterDetour, SpeedVa(kLanDefaultPortGetter), reinterpret_cast<void*>(&HookLanDefaultPort), 5, "default port getter 0x0056E460");
			LogLine("internal LAN hooks disabled; online host/port hooks installed");
			return;
		}
		LogLine("internal hooks disabled by config");
		return;
	}

	InstallInlineDetour(&gCtorDetour, SpeedVa(kLanSelectCtor), reinterpret_cast<void*>(&HookLanCtor), 7, "LANServerSelect::ctor 0x00560470");
	InstallInlineDetour(&gParseDetour, SpeedVa(kLanSelectParse), reinterpret_cast<void*>(&HookLanParse), 6, "LANServerSelect::parse 0x00558DF0");
	LONG selectedHostCfg = InterlockedCompareExchange(&gLanInternalSelectedHostHookEnabled, 0, 0);
	if (selectedHostCfg != 0) {
		InstallInlineDetour(&gSetHostDetour, SpeedVa(kLanSelectedHostSetter), reinterpret_cast<void*>(&HookLanSetHost), 6, "LAN selected-host setter 0x0056E480");
	} else {
		LogLine("lan-select selected-host hook skipped by config addr=0x%08lX", (unsigned long)kLanSelectedHostSetter);
	}
	LONG eventCfg = InterlockedCompareExchange(&gLanInternalEventHookEnabled, 0, 0);
	if (eventCfg != 0 || onlineOverrideCfg != 0) {
		InstallInlineDetour(&gHostGetterDetour, SpeedVa(kLanDefaultHostGetter), reinterpret_cast<void*>(&HookLanDefaultHost), 5, "default host getter 0x0056E440");
		InstallInlineDetour(&gPortGetterDetour, SpeedVa(kLanDefaultPortGetter), reinterpret_cast<void*>(&HookLanDefaultPort), 5, "default port getter 0x0056E460");
	} else {
		LogLine("default host/port hooks skipped by config");
	}
	InstallInlineDetour(&gLobbyJoinUsersetDetour, SpeedVa(kLobbyJoinUserset), reinterpret_cast<void*>(&HookLobbyJoinUserset), 5, "lobby join-userset 0x0079EE50");
	InstallInlineDetour(&gLobbyJoinNameDetour, SpeedVa(kLobbyJoinName), reinterpret_cast<void*>(&HookLobbyJoinName), 6, "lobby join-name 0x0079B260");
	InstallInlineDetour(&gLobbyUjoiCallbackDetour, SpeedVa(kLobbyUjoiCallback), reinterpret_cast<void*>(&HookLobbyUjoiCallback), 5, "lobby ujoi-callback 0x0079AA50");
	InstallInlineDetour(&gLobbyJoinAutoCallbackDetour, SpeedVa(kLobbyJoinAutoCallback), reinterpret_cast<void*>(&HookLobbyJoinAutoCallback), 7, "lobby join-auto-callback 0x0079B3A0");
	InstallInlineDetour(&gLobbyUsetPlayEventDetour, SpeedVa(kLobbyUsetPlayEvent), reinterpret_cast<void*>(&HookLobbyUsetPlayEvent), 5, "lobby uset/play event 0x0079B520");
	if (eventCfg != 0) {
		InstallInlineDetour(&gEventDetour, SpeedVa(kLanSelectEventEntry), reinterpret_cast<void*>(&HookLanEvent), 5, "LANServerSelect::event 0x005625C0");
		InstallInlineDetour(&gUiEventDetour, SpeedVa(kLanSelectUiEvent), reinterpret_cast<void*>(&HookLanUiEvent), 9, "LANServerSelect::ui-event 0x00560640");
		InstallInlineDetour(&gPcLanEventDetour, SpeedVa(kPcLanEvent), reinterpret_cast<void*>(&HookPcLanEvent), 9, "UI_PC_LAN::event 0x00559090");
		InstallInlineDetour(&gStartJoinDetour, SpeedVa(kLanSelectStartJoin), reinterpret_cast<void*>(&HookLanStartJoin), 9, "LANServerSelect::start-join 0x00560220");
		InstallInlineDetour(&gJoinBuildDetour, SpeedVa(kLanJoinFlowBuild), reinterpret_cast<void*>(&HookLanJoinBuild), 7, "LAN join-flow build 0x005538E0");
		InstallInlineDetour(&gJoinTickDetour, SpeedVa(kLanJoinFlowTick), reinterpret_cast<void*>(&HookLanJoinTick), 5, "LAN join-flow tick 0x00553980");
		InstallInlineDetour(&gNetConnectDetour, SpeedVa(kLanNetConnect), reinterpret_cast<void*>(&HookLanNetConnect), 5, "LAN net-connect 0x00792BF0");
		InstallInlineDetour(&gNetSessionConnectDetour, SpeedVa(kLanNetSessionConnect), reinterpret_cast<void*>(&HookLanNetSessionConnect), 5, "LAN net-session 0x00792CA0");
		InstallInlineDetour(&gNetPersonaConnectDetour, SpeedVa(kLanNetPersonaConnect), reinterpret_cast<void*>(&HookLanNetPersonaConnect), 5, "LAN net-persona 0x00792EF0");
		LogLine("LAN net-status hooks skipped: short conditional branches need a relocating trampoline");
	} else {
		LogLine("lan-select main-event hook skipped by config");
	}
}
