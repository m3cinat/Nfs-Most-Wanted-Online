#pragma once

#include <cstddef>
#include <cstdint>

struct InlineDetour {
	void* target = nullptr;
	void* replacement = nullptr;
	void* trampoline = nullptr;
	std::uint8_t original[16] = {};
	std::size_t patchLen = 0;
	bool installed = false;
	const char* name = nullptr;
};

bool InstallInlineDetour(InlineDetour* detour, void* target, void* replacement, std::size_t patchLen, const char* name);
