#pragma once

#include <winsock2.h>
#include <windows.h>
#include <string>

void InitLogging();
void ShutdownLogging();
void LogLine(const char* fmt, ...);
void LoggerSetVerboseData(bool enabled);
bool LoggerVerboseData();
std::string AddrToString(const sockaddr* sa, int salen);
void LogDataPreview(const char* tag, const char* buf, int len);
void LogDataFullHex(const char* tag, const char* buf, int len);
