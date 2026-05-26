#include "detour.h"

#include "logger.h"

#include <cstring>

static bool WriteJump(void* at, void* to, std::size_t patchLen)
{
	if (!at || !to || patchLen < 5)
		return false;

	DWORD oldProtect = 0;
	if (!VirtualProtect(at, patchLen, PAGE_EXECUTE_READWRITE, &oldProtect))
		return false;

	std::uint8_t* p = reinterpret_cast<std::uint8_t*>(at);
	std::uintptr_t rel = reinterpret_cast<std::uintptr_t>(to) - (reinterpret_cast<std::uintptr_t>(at) + 5);
	p[0] = 0xE9;
	std::memcpy(p + 1, &rel, 4);
	for (std::size_t i = 5; i < patchLen; ++i)
		p[i] = 0x90;

	DWORD tmp = 0;
	VirtualProtect(at, patchLen, oldProtect, &tmp);
	FlushInstructionCache(GetCurrentProcess(), at, patchLen);
	return true;
}

bool InstallInlineDetour(InlineDetour* detour, void* target, void* replacement, std::size_t patchLen, const char* name)
{
	if (!detour || !target || !replacement || patchLen < 5 || patchLen > sizeof(detour->original))
		return false;
	if (detour->installed)
		return true;

	void* trampoline = VirtualAlloc(nullptr, patchLen + 5, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!trampoline) {
		LogLine("detour install failed name=%s target=%p reason=VirtualAlloc", name ? name : "-", target);
		return false;
	}

	std::memcpy(detour->original, target, patchLen);
	std::memcpy(trampoline, target, patchLen);
	void* continueAt = reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(target) + patchLen);
	if (!WriteJump(reinterpret_cast<std::uint8_t*>(trampoline) + patchLen, continueAt, 5)) {
		VirtualFree(trampoline, 0, MEM_RELEASE);
		LogLine("detour install failed name=%s target=%p reason=trampoline-jump", name ? name : "-", target);
		return false;
	}
	if (!WriteJump(target, replacement, patchLen)) {
		VirtualFree(trampoline, 0, MEM_RELEASE);
		LogLine("detour install failed name=%s target=%p reason=target-jump", name ? name : "-", target);
		return false;
	}

	detour->target = target;
	detour->replacement = replacement;
	detour->trampoline = trampoline;
	detour->patchLen = patchLen;
	detour->installed = true;
	detour->name = name;
	LogLine("detour installed name=%s target=%p replacement=%p trampoline=%p patch_len=%u", name ? name : "-", target, replacement, trampoline, (unsigned)patchLen);
	return true;
}
