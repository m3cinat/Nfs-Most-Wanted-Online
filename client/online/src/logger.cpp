#include "logger.h"

#include <ws2tcpip.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

static FILE* gLog = nullptr;
static CRITICAL_SECTION gLogLock;
static bool gLogLockReady = false;
static bool gVerboseData = false;

void LoggerSetVerboseData(bool enabled)
{
	gVerboseData = enabled;
}

bool LoggerVerboseData()
{
	return gVerboseData;
}

void LogLine(const char* fmt, ...)
{
	if (!gLog)
		return;
	if (gLogLockReady)
		EnterCriticalSection(&gLogLock);

	SYSTEMTIME st{};
	GetLocalTime(&st);
	std::fprintf(
		gLog,
		"[%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%lu] ",
		(unsigned)st.wYear,
		(unsigned)st.wMonth,
		(unsigned)st.wDay,
		(unsigned)st.wHour,
		(unsigned)st.wMinute,
		(unsigned)st.wSecond,
		(unsigned)st.wMilliseconds,
		(unsigned long)GetCurrentProcessId());

	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(gLog, fmt, ap);
	va_end(ap);
	std::fprintf(gLog, "\n");
	std::fflush(gLog);

	if (gLogLockReady)
		LeaveCriticalSection(&gLogLock);
}

std::string AddrToString(const sockaddr* sa, int salen)
{
	if (!sa || salen <= 0)
		return "<null>";

	char host[128] = {};
	char serv[32] = {};
	int rr = getnameinfo(sa, salen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
	if (rr == 0)
		return std::string(host) + ":" + serv;

	if (sa->sa_family == AF_INET && salen >= (int)sizeof(sockaddr_in)) {
		const sockaddr_in* si = reinterpret_cast<const sockaddr_in*>(sa);
		char ip[64] = {};
		inet_ntop(AF_INET, &si->sin_addr, ip, sizeof(ip));
		char out[96] = {};
		std::snprintf(out, sizeof(out), "%s:%u", ip[0] ? ip : "0.0.0.0", (unsigned)ntohs(si->sin_port));
		return out;
	}
	if (sa->sa_family == AF_INET6 && salen >= (int)sizeof(sockaddr_in6)) {
		const sockaddr_in6* si6 = reinterpret_cast<const sockaddr_in6*>(sa);
		char ip[128] = {};
		inet_ntop(AF_INET6, &si6->sin6_addr, ip, sizeof(ip));
		char out[180] = {};
		std::snprintf(out, sizeof(out), "[%s]:%u", ip[0] ? ip : "::", (unsigned)ntohs(si6->sin6_port));
		return out;
	}
	char out[64] = {};
	std::snprintf(out, sizeof(out), "af=%d", (int)sa->sa_family);
	return out;
}

void LogDataPreview(const char* tag, const char* buf, int len)
{
	if (!buf || len <= 0)
		return;
	int n = len < 96 ? len : 96;
	char hex[96 * 3 + 1] = {};
	char ascii[96 + 1] = {};
	for (int i = 0; i < n; ++i) {
		unsigned char c = (unsigned char)buf[i];
		std::snprintf(hex + i * 3, sizeof(hex) - (size_t)i * 3, "%02X ", (unsigned)c);
		ascii[i] = (c >= 32 && c <= 126) ? (char)c : '.';
	}
	LogLine("%s data len=%d first=%d hex=%s ascii=%s", tag ? tag : "packet", len, n, hex, ascii);
}

void LogDataFullHex(const char* tag, const char* buf, int len)
{
	if (!buf || len <= 0)
		return;
	char line[160] = {};
	for (int off = 0; off < len; off += 32) {
		int n = (len - off) < 32 ? (len - off) : 32;
		char hex[32 * 3 + 1] = {};
		for (int i = 0; i < n; ++i) {
			unsigned char c = (unsigned char)buf[off + i];
			std::snprintf(hex + i * 3, sizeof(hex) - (size_t)i * 3, "%02X ", (unsigned)c);
		}
		std::snprintf(line, sizeof(line), "%s full off=%03d hex=%s", tag ? tag : "packet", off, hex);
		LogLine("%s", line);
	}
}

void InitLogging()
{
	if (!gLogLockReady) {
		InitializeCriticalSection(&gLogLock);
		gLogLockReady = true;
	}
	char path[MAX_PATH] = {};
	GetModuleFileNameA(nullptr, path, sizeof(path));
	char* slash = std::strrchr(path, '\\');
	if (slash)
		*(slash + 1) = '\0';
	std::strcat(path, "MWOnline.log");
	gLog = std::fopen(path, "ab");
	if (gLog)
		LogLine("logger start");
}

void ShutdownLogging()
{
	if (gLog)
		LogLine("logger stop");
	if (gLog) {
		std::fclose(gLog);
		gLog = nullptr;
	}
	if (gLogLockReady) {
		DeleteCriticalSection(&gLogLock);
		gLogLockReady = false;
	}
}
