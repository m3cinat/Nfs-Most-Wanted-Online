#pragma once

#include <cstdint>

void DumpLanSelectCore(const char* tag, void* self);
int DumpLanServerList(const char* tag, void* self);
bool InjectFakeLanServerEntry(void* self);
const char* LanSelectEventName(std::uint32_t hash);
void LogLanSelectEvent(const char* tag, void* self, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4);
void LogLanSelectJoinState(const char* tag, void* self);
