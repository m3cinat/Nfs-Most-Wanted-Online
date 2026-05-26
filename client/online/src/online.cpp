#include <winsock2.h>
#include <windows.h>

#include "config.h"
#include "internal_hooks.h"
#include "logger.h"
#include "winsock_hooks.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

static HMODULE gSelf = nullptr;
static volatile LONG gCertPatchApplied = 0;

static bool ProtectWrite(void* dst, const void* src, std::size_t len)
{
	if (!dst || !src || len == 0)
		return false;
	DWORD oldProtect = 0;
	if (!VirtualProtect(dst, len, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;
	std::memcpy(dst, src, len);
	DWORD tmp = 0;
	VirtualProtect(dst, len, oldProtect, &tmp);
	FlushInstructionCache(GetCurrentProcess(), dst, len);
	return true;
}

static std::uint8_t* FindMaskedPattern(std::uint8_t* base, std::size_t size, const std::uint8_t* pat, const bool* mask, std::size_t len)
{
	if (!base || !pat || !mask || len == 0 || size < len)
		return nullptr;
	for (std::size_t i = 0; i + len <= size; ++i) {
		bool ok = true;
		for (std::size_t j = 0; j < len; ++j) {
			if (mask[j] && base[i + j] != pat[j]) {
				ok = false;
				break;
			}
		}
		if (ok)
			return base + i;
	}
	return nullptr;
}

static bool ApplyCertPatch()
{
	if (InterlockedCompareExchange(&gCertPatchEnabled, 0, 0) == 0) {
		LogLine("cert patch skipped by config");
		return true;
	}
	if (InterlockedCompareExchange(&gCertPatchApplied, 1, 0) != 0)
		return true;

	std::uint8_t* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleA(nullptr));
	if (!base) {
		LogLine("cert patch skipped: exe base missing");
		return false;
	}
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
		LogLine("cert patch skipped: bad dos header");
		return false;
	}
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) {
		LogLine("cert patch skipped: bad nt header");
		return false;
	}

	const std::uint8_t pat[] = {
		0x7D, 0x00, 0xC7, 0x86, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xEB, 0x00, 0x03, 0x7C, 0x24,
	};
	const std::uint8_t patchedPat[] = {
		0x7E, 0x00, 0xC7, 0x86, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0xEB, 0x00, 0x03, 0x7C, 0x24,
	};
	const bool mask[] = {
		true, false, true, true, false, false, false, false,
		false, false, false, false, true, false, true, true, true,
	};
	std::size_t size = nt->OptionalHeader.SizeOfImage;
	std::uint8_t* match = FindMaskedPattern(base, size, pat, mask, sizeof(pat));
	if (!match)
		match = FindMaskedPattern(base, size, patchedPat, mask, sizeof(patchedPat));
	if (!match) {
		LogLine("cert patch missing: ssl branch pattern not found");
		return false;
	}
	if (match[0] == 0x7E) {
		LogLine("cert patch already applied at %p", match);
		return true;
	}
	std::uint8_t jump = 0x7E;
	bool ok = ProtectWrite(match, &jump, sizeof(jump));
	LogLine("cert patch %s at %p old=0x%02X new=0x%02X", ok ? "applied" : "failed", match, 0x7D, jump);
	return ok;
}

static DWORD WINAPI MainThread(LPVOID)
{
	WSADATA wsa{};
	WSAStartup(MAKEWORD(2, 2), &wsa);
	InitLogging();
	LoadConfig(gSelf);
	LogLine(
		"config online_override=%ld cert_patch=%ld lan_override=%ld lan_discovery_mirror=%ld lan_discovery_inject=%ld internal_hooks=%ld internal_fake=%ld internal_event=%ld internal_refresh_state=%ld internal_selected_host=%ld internal_force_open_selected=%ld online=%s:%u race=%s:%u lan=%s:%u control=%s:%u alias=%s:%u discovery=%s:%u fake='%s' fake_short=%u fake_id=0x%08lX",
		(long)InterlockedCompareExchange(&gOnlineOverrideEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gCertPatchEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanDiscoveryMirrorEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanDiscoveryInjectEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalHooksEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalFakeEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalEventHookEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalRefreshStateEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalSelectedHostHookEnabled, 0, 0),
		(long)InterlockedCompareExchange(&gLanInternalForceOpenSelectedEnabled, 0, 0),
		gOnline.host,
		(unsigned)gOnline.port,
		gRace.host,
		(unsigned)gRace.port,
		gLan.host,
		(unsigned)gLan.port,
		gLanControl.host,
		(unsigned)gLanControl.port,
		gLanControlAlias.host,
		(unsigned)gLanControlAlias.port,
		gLanDiscovery.host,
		(unsigned)gLanDiscovery.port,
		gLanInternalFakeName,
		(unsigned)gLanInternalFakeShort,
		(unsigned long)gLanInternalFakeId);

	ApplyCertPatch();
	InitWinsockHooks(gSelf);
	InitInternalHooks();
	LogLine("hooks installed");
	return 0;
}

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		gSelf = mod;
		DisableThreadLibraryCalls(mod);
		CreateThread(nullptr, 0, &MainThread, nullptr, 0, nullptr);
	} else if (reason == DLL_PROCESS_DETACH) {
		ShutdownWinsockHooks();
		ShutdownLogging();
	}
	return TRUE;
}
