#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <string>

#include "config.h"
#include "logger.h"
#include "winsock_hooks.h"

#pragma comment(lib, "ws2_32.lib")

using ConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
using WSAConnectFn = int (WSAAPI*)(SOCKET, const sockaddr*, int, LPWSABUF, LPWSABUF, LPQOS, LPQOS);
using SendFn = int (WSAAPI*)(SOCKET, const char*, int, int);
using RecvFn = int (WSAAPI*)(SOCKET, char*, int, int);
using SendToFn = int (WSAAPI*)(SOCKET, const char*, int, int, const sockaddr*, int);
using RecvFromFn = int (WSAAPI*)(SOCKET, char*, int, int, sockaddr*, int*);
using WSASendFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSARecvFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSASendToFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, DWORD, const sockaddr*, int, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using WSARecvFromFn = int (WSAAPI*)(SOCKET, LPWSABUF, DWORD, LPDWORD, LPDWORD, sockaddr*, LPINT, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
using BindFn = int (WSAAPI*)(SOCKET, const sockaddr*, int);
using GetSockNameFn = int (WSAAPI*)(SOCKET, sockaddr*, int*);
using GetPeerNameFn = int (WSAAPI*)(SOCKET, sockaddr*, int*);
using CloseSocketFn = int (WSAAPI*)(SOCKET);
using SocketFn = SOCKET (WSAAPI*)(int, int, int);
using WSASocketAFn = SOCKET (WSAAPI*)(int, int, int, LPWSAPROTOCOL_INFOA, GROUP, DWORD);
using GetProcAddressFn = FARPROC (WINAPI*)(HMODULE, LPCSTR);

static HMODULE gSelf = nullptr;

static ConnectFn gRealConnect = nullptr;
static WSAConnectFn gRealWSAConnect = nullptr;
static SendFn gRealSend = nullptr;
static RecvFn gRealRecv = nullptr;
static SendToFn gRealSendTo = nullptr;
static RecvFromFn gRealRecvFrom = nullptr;
static WSASendFn gRealWSASend = nullptr;
static WSARecvFn gRealWSARecv = nullptr;
static WSASendToFn gRealWSASendTo = nullptr;
static WSARecvFromFn gRealWSARecvFrom = nullptr;
static BindFn gRealBind = nullptr;
static GetSockNameFn gRealGetSockName = nullptr;
static GetPeerNameFn gRealGetPeerName = nullptr;
static CloseSocketFn gRealCloseSocket = nullptr;
static SocketFn gRealSocket = nullptr;
static WSASocketAFn gRealWSASocketA = nullptr;
static GetProcAddressFn gRealGetProcAddress = nullptr;


static SOCKET WSAAPI MySocket(int af, int type, int protocol);
static SOCKET WSAAPI MyWSASocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags);
static int WSAAPI MyBind(SOCKET s, const sockaddr* name, int namelen);
static int WSAAPI MyConnect(SOCKET s, const sockaddr* name, int namelen);
static int WSAAPI MyWSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF caller, LPWSABUF callee, LPQOS sqos, LPQOS gqos);
static int WSAAPI MySend(SOCKET s, const char* buf, int len, int flags);
static int WSAAPI MyRecv(SOCKET s, char* buf, int len, int flags);
static int WSAAPI MySendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
static int WSAAPI MyRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);
static int WSAAPI MyWSASend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb);
static int WSAAPI MyWSARecv(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD recvd, LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb);
static int WSAAPI MyWSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb);
static int WSAAPI MyWSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD recvd, LPDWORD flags, sockaddr* from, LPINT fromlen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb);
static int WSAAPI MyGetSockName(SOCKET s, sockaddr* name, int* namelen);
static int WSAAPI MyGetPeerName(SOCKET s, sockaddr* name, int* namelen);
static int WSAAPI MyCloseSocket(SOCKET s);
static FARPROC WINAPI MyGetProcAddress(HMODULE mod, LPCSTR proc);
static DWORD WINAPI HookRefreshThread(LPVOID);

struct SockMeta {
	SOCKET s = INVALID_SOCKET;
	int af = AF_UNSPEC;
	int type = 0;
	int proto = 0;
};

static SockMeta gSockMeta[256] = {};
static CRITICAL_SECTION gSockMetaLock;
static bool gSockMetaLockReady = false;

struct SyntheticLanState {
	SOCKET s = INVALID_SOCKET;
	bool sent = false;
	bool pendingEmpty = false;
	bool pendingEntry = false;
	sockaddr_in lastSource = {};
	sockaddr_in pendingEntrySource = {};
	char pendingEntryBuf[384] = {};
};

static SyntheticLanState gSyntheticLan[64] = {};
static CRITICAL_SECTION gSyntheticLanLock;
static bool gSyntheticLanLockReady = false;

static const char* SockTypeName(int type)
{
	switch (type) {
	case SOCK_STREAM: return "STREAM";
	case SOCK_DGRAM: return "DGRAM";
	case SOCK_RAW: return "RAW";
	default: return "OTHER";
	}
}

static void SockMetaSet(SOCKET s, int af, int type, int proto)
{
	if (!gSockMetaLockReady)
		return;
	EnterCriticalSection(&gSockMetaLock);
	for (auto& m : gSockMeta) {
		if (m.s == s || m.s == INVALID_SOCKET) {
			m.s = s;
			m.af = af;
			m.type = type;
			m.proto = proto;
			break;
		}
	}
	LeaveCriticalSection(&gSockMetaLock);
}

static SockMeta SockMetaGet(SOCKET s)
{
	SockMeta out{};
	out.s = INVALID_SOCKET;
	if (!gSockMetaLockReady)
		return out;
	EnterCriticalSection(&gSockMetaLock);
	for (const auto& m : gSockMeta) {
		if (m.s == s) {
			out = m;
			break;
		}
	}
	LeaveCriticalSection(&gSockMetaLock);
	return out;
}

static void SockMetaClear(SOCKET s)
{
	if (!gSockMetaLockReady)
		return;
	EnterCriticalSection(&gSockMetaLock);
	for (auto& m : gSockMeta) {
		if (m.s == s) {
			m = SockMeta{};
			m.s = INVALID_SOCKET;
			break;
		}
	}
	LeaveCriticalSection(&gSockMetaLock);
}

static bool SyntheticLanAlreadySent(SOCKET s)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET)
		return true;
	EnterCriticalSection(&gSyntheticLanLock);
	int freeSlot = -1;
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s) {
			bool sent = gSyntheticLan[i].sent;
			gSyntheticLan[i].sent = true;
			LeaveCriticalSection(&gSyntheticLanLock);
			return sent;
		}
		if (freeSlot < 0 && (gSyntheticLan[i].s == INVALID_SOCKET || gSyntheticLan[i].s == 0))
			freeSlot = i;
	}
	if (freeSlot >= 0) {
		gSyntheticLan[freeSlot].s = s;
		gSyntheticLan[freeSlot].sent = true;
	}
	LeaveCriticalSection(&gSyntheticLanLock);
	return false;
}

static void SyntheticLanSetPendingEmpty(SOCKET s, const sockaddr_in* src)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET || !src)
		return;
	EnterCriticalSection(&gSyntheticLanLock);
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s) {
			gSyntheticLan[i].pendingEmpty = true;
			gSyntheticLan[i].lastSource = *src;
			break;
		}
	}
	LeaveCriticalSection(&gSyntheticLanLock);
}

static bool SyntheticLanTakePendingEmpty(SOCKET s, sockaddr_in* src)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET || !src)
		return false;
	EnterCriticalSection(&gSyntheticLanLock);
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s && gSyntheticLan[i].pendingEmpty) {
			gSyntheticLan[i].pendingEmpty = false;
			*src = gSyntheticLan[i].lastSource;
			LeaveCriticalSection(&gSyntheticLanLock);
			return true;
		}
	}
	LeaveCriticalSection(&gSyntheticLanLock);
	return false;
}

static void SyntheticLanSetPendingEntry(SOCKET s, const char* packet, const sockaddr_in* src)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET || !packet || !src)
		return;
	EnterCriticalSection(&gSyntheticLanLock);
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s) {
			std::memcpy(gSyntheticLan[i].pendingEntryBuf, packet, 384);
			gSyntheticLan[i].pendingEntrySource = *src;
			gSyntheticLan[i].pendingEntry = true;
			break;
		}
	}
	LeaveCriticalSection(&gSyntheticLanLock);
}

static bool SyntheticLanTakePendingEntry(SOCKET s, char* packet, sockaddr_in* src)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET || !packet || !src)
		return false;
	EnterCriticalSection(&gSyntheticLanLock);
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s && gSyntheticLan[i].pendingEntry) {
			std::memcpy(packet, gSyntheticLan[i].pendingEntryBuf, 384);
			*src = gSyntheticLan[i].pendingEntrySource;
			gSyntheticLan[i].pendingEntry = false;
			LeaveCriticalSection(&gSyntheticLanLock);
			return true;
		}
	}
	LeaveCriticalSection(&gSyntheticLanLock);
	return false;
}

static void SyntheticLanClear(SOCKET s)
{
	if (!gSyntheticLanLockReady || s == INVALID_SOCKET)
		return;
	EnterCriticalSection(&gSyntheticLanLock);
	for (int i = 0; i < 64; ++i) {
		if (gSyntheticLan[i].s == s) {
			gSyntheticLan[i] = SyntheticLanState{};
			gSyntheticLan[i].s = INVALID_SOCKET;
			break;
		}
	}
	LeaveCriticalSection(&gSyntheticLanLock);
}

static void LogSocketName(const char* prefix, SOCKET s)
{
	if (!gRealGetSockName && !gRealGetPeerName)
		return;
	sockaddr_storage ss{};
	int slen = sizeof(ss);
	if (gRealGetSockName && gRealGetSockName(s, reinterpret_cast<sockaddr*>(&ss), &slen) == 0)
		LogLine("%s local=%s", prefix, AddrToString(reinterpret_cast<sockaddr*>(&ss), slen).c_str());
	slen = sizeof(ss);
	if (gRealGetPeerName && gRealGetPeerName(s, reinterpret_cast<sockaddr*>(&ss), &slen) == 0)
		LogLine("%s peer=%s", prefix, AddrToString(reinterpret_cast<sockaddr*>(&ss), slen).c_str());
}

static bool IsConfiguredTcpPort(std::uint16_t port)
{
	return port == 9900 ||
		port == 20921 ||
		port == 20922 ||
		port == 20923 ||
		port == 13505 ||
		port == gLan.port ||
		port == gLanControl.port ||
		port == gLanControlAlias.port;
}

static bool ShouldLogTcpStream(SOCKET s, const SockMeta& meta)
{
	if (meta.type != SOCK_STREAM && meta.type != 0)
		return false;
	if (LoggerVerboseData())
		return true;
	if (!gRealGetPeerName)
		return true;
	sockaddr_storage ss{};
	int slen = sizeof(ss);
	if (gRealGetPeerName(s, reinterpret_cast<sockaddr*>(&ss), &slen) != 0)
		return true;
	if (ss.ss_family == AF_INET && slen >= (int)sizeof(sockaddr_in)) {
		const sockaddr_in* si = reinterpret_cast<const sockaddr_in*>(&ss);
		return IsConfiguredTcpPort(ntohs(si->sin_port));
	}
	return false;
}

static void LogTcpStreamData(const char* tag, SOCKET s, const char* buf, int len, int flags, int rr, int wsa, DWORD count, bool overlapped)
{
	SockMeta meta = SockMetaGet(s);
	if (!ShouldLogTcpStream(s, meta))
		return;
	LogLine(
		"%s s=0x%Ix type=%s len=%d rr=%d flags=0x%X count=%lu ov=%d wsa=%d",
		tag ? tag : "tcp",
		static_cast<size_t>(s),
		SockTypeName(meta.type),
		len,
		rr,
		flags,
		(unsigned long)count,
		overlapped ? 1 : 0,
		wsa);
	LogSocketName(tag ? tag : "tcp", s);
	if (buf && len > 0)
		LogDataPreview(tag ? tag : "tcp", buf, len);
}

static bool IsLanRedirectPort(std::uint16_t port, const Endpoint** ep, const char** name)
{
	bool internalLan = InterlockedCompareExchange(&gLanInternalHooksEnabled, 0, 0) != 0;
	if ((!internalLan && port == 9900) || port == gLan.port) {
		if (ep) *ep = &gLan;
		if (name) *name = "lan_lobby";
		return true;
	}
	if (port == 20923 || port == gLanControl.port) {
		if (ep) *ep = &gLanControl;
		if (name) *name = "lan_control";
		return true;
	}
	if (port == 13505 || port == gLanControlAlias.port) {
		if (ep) *ep = &gLanControlAlias;
		if (name) *name = "lan_control_alias";
		return true;
	}
	return false;
}

static bool BuildLanTcpRedirect(const sockaddr* name, int namelen, sockaddr_in* out, const char** epName)
{
	if (InterlockedCompareExchange(&gLanOverrideEnabled, 0, 0) == 0 ||
		!name || namelen < (int)sizeof(sockaddr_in) || name->sa_family != AF_INET || !out)
		return false;

	const sockaddr_in* si = reinterpret_cast<const sockaddr_in*>(name);
	const Endpoint* ep = nullptr;
	const char* label = nullptr;
	if (!IsLanRedirectPort(ntohs(si->sin_port), &ep, &label) || !ep || ep->port == 0)
		return false;

	sockaddr_in dst = {};
	if (!ResolveTo(ep->host, ep->port, &dst))
		return false;
	if (dst.sin_addr.s_addr == si->sin_addr.s_addr && dst.sin_port == si->sin_port)
		return false;

	*out = dst;
	if (epName)
		*epName = label;
	return true;
}

static bool IsBroadcastOrMulticast(const sockaddr* to, int tolen)
{
	if (!to || tolen < (int)sizeof(sockaddr_in) || to->sa_family != AF_INET)
		return false;
	const sockaddr_in* si = reinterpret_cast<const sockaddr_in*>(to);
	unsigned long host = ntohl(si->sin_addr.s_addr);
	return si->sin_addr.s_addr == INADDR_BROADCAST ||
		host == 0xFFFFFFFFu ||
		(host >= 0xE0000000u && host <= 0xEFFFFFFFu);
}

static bool LooksLikeLanDiscoveryPacket(const char* buf, int len);

static bool BuildRaceUdpRedirect(const char* buf, int len, const sockaddr* to, int tolen, sockaddr_in* out)
{
	if (!to || tolen < (int)sizeof(sockaddr_in) || to->sa_family != AF_INET || !out)
		return false;
	if (gRace.port == 0 || gRace.host[0] == 0)
		return false;
	if (IsBroadcastOrMulticast(to, tolen))
		return false;
	if (LooksLikeLanDiscoveryPacket(buf, len))
		return false;

	const sockaddr_in* si = reinterpret_cast<const sockaddr_in*>(to);
	if (si->sin_addr.s_addr == gRaceAddr.sin_addr.s_addr && si->sin_port == gRaceAddr.sin_port)
		return false;

	sockaddr_in dst = {};
	if (!ResolveTo(gRace.host, gRace.port, &dst))
		return false;
	*out = dst;
	return true;
}

static void CopyLanField(char* out, size_t outSize, const char* buf, int len, int off, int width)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	if (!buf || len <= off || width <= 0)
		return;
	int n = len - off;
	if (n > width)
		n = width;
	size_t j = 0;
	for (int i = 0; i < n && j + 1 < outSize; ++i) {
		unsigned char c = (unsigned char)buf[off + i];
		if (c == 0)
			break;
		out[j++] = (c >= 32 && c <= 126) ? (char)c : '.';
	}
	out[j] = 0;
}

static const char* LanDiscoveryKind(const char* buf, int len)
{
	if (!buf || len < 384 || buf[0] != 'g' || buf[1] != 'E' || buf[2] != 'A')
		return "not-lan";
	if ((unsigned char)buf[3] == 0x03)
		return "server-entry";
	if ((unsigned char)buf[3] == 0x00 && (unsigned char)buf[8] == 0x3F)
		return "empty";
	return "query-or-other";
}

static bool InternalLanOwnsList()
{
	return InterlockedCompareExchange(&gLanInternalHooksEnabled, 0, 0) != 0 &&
		InterlockedCompareExchange(&gLanInternalFakeEnabled, 0, 0) != 0;
}

static void LogLanDiscoveryPacket(const char* tag, const char* buf, int len, const sockaddr* addr, int addrLen, const char* note)
{
	if (!buf || len <= 0)
		return;
	char hostId[40] = {};
	char name[40] = {};
	char portField[48] = {};
	char proto[128] = {};
	CopyLanField(hostId, sizeof(hostId), buf, len, 0x08, 0x20);
	CopyLanField(name, sizeof(name), buf, len, 0x28, 0x20);
	CopyLanField(portField, sizeof(portField), buf, len, 0x48, 0xc0);
	CopyLanField(proto, sizeof(proto), buf, len, 0x108, 0x78);
	std::string addrText = (addr && addrLen > 0) ? AddrToString(addr, addrLen) : std::string("<none>");
	LogLine(
		"%s lan-discovery kind=%s len=%d fromto=%s stamp=%02X%02X%02X%02X ttl=%u hostid=%s name=%s port_field=%s proto=%s%s%s",
		tag ? tag : "packet",
		LanDiscoveryKind(buf, len),
		len,
		addrText.c_str(),
		len > 4 ? (unsigned char)buf[4] : 0,
		len > 5 ? (unsigned char)buf[5] : 0,
		len > 6 ? (unsigned char)buf[6] : 0,
		len > 7 ? (unsigned char)buf[7] : 0,
		len > 3 ? (unsigned)(unsigned char)buf[3] : 0,
		hostId[0] ? hostId : "-",
		name[0] ? name : "-",
		portField[0] ? portField : "-",
		proto[0] ? proto : "-",
		note && note[0] ? " note=" : "",
		note && note[0] ? note : "");
	LogDataPreview(tag ? tag : "lan-discovery", buf, len);
	if (len == 384)
		LogDataFullHex(tag ? tag : "lan-discovery", buf, len);
}

static bool LooksLikeLanServerEntry(const char* buf, int len);

static bool FillSyntheticLanDiscovery(SOCKET s, char* buf, int len, sockaddr* from, int* fromlen, bool preserveSource, bool allowRepeat)
{
	if (InterlockedCompareExchange(&gLanDiscoveryInjectEnabled, 0, 0) == 0 ||
		InternalLanOwnsList() ||
		!buf || len < 384 || !from || !fromlen ||
		gLan.host[0] == 0 || gLan.port == 0)
		return false;
	if (allowRepeat) {
		SyntheticLanAlreadySent(s);
	} else if (SyntheticLanAlreadySent(s)) {
		return false;
	}

	sockaddr_in src = {};
	if (preserveSource && *fromlen >= (int)sizeof(sockaddr_in)) {
		std::memcpy(&src, from, sizeof(src));
		if (src.sin_family == AF_INET && ntohs(src.sin_port) == 9999) {
			src.sin_port = htons((std::uint16_t)(50000u + (GetTickCount() % 10000u)));
		}
	} else if (!ResolveTo(gLan.host, gLanDiscovery.port ? gLanDiscovery.port : 9999, &src)) {
		ResolveTo("127.0.0.1", gLanDiscovery.port ? gLanDiscovery.port : 9999, &src);
	}

	DWORD stamp = 0;
	if (len > 7) {
		stamp =
			((DWORD)(unsigned char)buf[4] << 24) |
			((DWORD)(unsigned char)buf[5] << 16) |
			((DWORD)(unsigned char)buf[6] << 8) |
			((DWORD)(unsigned char)buf[7]);
	}
	if (stamp == 0)
		stamp = GetTickCount();

	std::memset(buf, 0, 384);
	buf[0] = 'g';
	buf[1] = 'E';
	buf[2] = 'A';
	buf[3] = 0x03;
	buf[4] = (char)((stamp >> 24) & 0xFF);
	buf[5] = (char)((stamp >> 16) & 0xFF);
	buf[6] = (char)((stamp >> 8) & 0xFF);
	buf[7] = (char)(stamp & 0xFF);

	char hostId[32] = {};
	lstrcpynA(hostId, gLanDiscoveryHostId[0] ? gLanDiscoveryHostId : "NFSMWNA", sizeof(hostId));
	char displayName[32] = {};
	lstrcpynA(displayName, gLanDiscoveryName[0] ? gLanDiscoveryName : "MWONLINE", sizeof(displayName));
	std::memset(buf + 8, 0, 32);
	std::memcpy(buf + 8, hostId, std::strlen(hostId));

	std::memset(buf + 40, 0, 32);
	std::memcpy(buf + 40, displayName, std::strlen(displayName));

	char portField[32] = {};
	std::snprintf(portField, sizeof(portField), "9900|0");
	std::memset(buf + 72, 0, 32);
	std::memcpy(buf + 72, portField, std::strlen(portField));

	const char* protoField = "TCP:~1:1024\tUDP:~1:1024";
	std::memcpy(buf + 264, protoField, std::strlen(protoField));

	std::memcpy(from, &src, sizeof(src));
	*fromlen = sizeof(src);
	SyntheticLanSetPendingEmpty(s, &src);
	LogLine(
		"lan discovery injected name=%s source=%s port_field=%s preserve_source=%d repeat=%d",
		displayName,
		AddrToString((sockaddr*)&src, sizeof(src)).c_str(),
		portField,
		preserveSource ? 1 : 0,
		allowRepeat ? 1 : 0);
	LogLanDiscoveryPacket("lan discovery injected", buf, 384, reinterpret_cast<const sockaddr*>(&src), sizeof(src), preserveSource ? "replace-real" : "synthetic-first");
	return true;
}

static bool QueueSyntheticLanCloneFromReal(SOCKET s, const char* buf, int len, const sockaddr* from, int fromlen)
{
	if (InterlockedCompareExchange(&gLanDiscoveryInjectEnabled, 0, 0) == 0 ||
		InternalLanOwnsList() ||
		!buf || len < 384 || !from || fromlen < (int)sizeof(sockaddr_in))
		return false;
	if (!LooksLikeLanServerEntry(buf, len))
		return false;

	char displayName[32] = {};
	lstrcpynA(displayName, gLanDiscoveryName[0] ? gLanDiscoveryName : "MWONLINE", sizeof(displayName));
	if (displayName[0] == '\0')
		return false;

	char existingName[40] = {};
	CopyLanField(existingName, sizeof(existingName), buf, len, 0x28, 0x20);
	if (_stricmp(existingName, displayName) == 0)
		return false;

	char clone[384] = {};
	std::memcpy(clone, buf, 384);
	DWORD stamp = GetTickCount();
	clone[4] = (char)((stamp >> 24) & 0xFF);
	clone[5] = (char)((stamp >> 16) & 0xFF);
	clone[6] = (char)((stamp >> 8) & 0xFF);
	clone[7] = (char)(stamp & 0xFF);
	std::memset(clone + 40, 0, 32);
	std::memcpy(clone + 40, displayName, std::strlen(displayName));

	sockaddr_in src = {};
	std::memcpy(&src, from, sizeof(src));
	SyntheticLanAlreadySent(s);
	SyntheticLanSetPendingEntry(s, clone, &src);
	LogLine("lan discovery queued clone name=%s source=%s", displayName, AddrToString((sockaddr*)&src, sizeof(src)).c_str());
	LogLanDiscoveryPacket("lan discovery queued clone", clone, 384, reinterpret_cast<const sockaddr*>(&src), sizeof(src), "clone-real-entry");
	return true;
}

static bool FillPendingSyntheticLanEntry(SOCKET s, char* buf, int len, sockaddr* from, int* fromlen)
{
	if (!buf || len < 384 || !from || !fromlen)
		return false;
	sockaddr_in src = {};
	if (!SyntheticLanTakePendingEntry(s, buf, &src))
		return false;
	std::memcpy(from, &src, sizeof(src));
	*fromlen = sizeof(src);
	LogLanDiscoveryPacket("lan discovery injected queued clone", buf, 384, reinterpret_cast<const sockaddr*>(&src), sizeof(src), "pending-clone");
	return true;
}

static bool FillSyntheticLanEmptyDiscovery(SOCKET s, char* buf, int len, sockaddr* from, int* fromlen)
{
	if (!buf || len < 384 || !from || !fromlen)
		return false;
	sockaddr_in src = {};
	if (!SyntheticLanTakePendingEmpty(s, &src))
		return false;
	std::memset(buf, 0, 384);
	buf[0] = 'g';
	buf[1] = 'E';
	buf[2] = 'A';
	buf[8] = 0x3F;
	std::memcpy(from, &src, sizeof(src));
	*fromlen = sizeof(src);
	LogLine("lan discovery injected empty source=%s", AddrToString((sockaddr*)&src, sizeof(src)).c_str());
	LogLanDiscoveryPacket("lan discovery injected empty", buf, 384, reinterpret_cast<const sockaddr*>(&src), sizeof(src), "empty-after-entry");
	return true;
}

static bool LooksLikeLanDiscoveryPacket(const char* buf, int len)
{
	return buf && len >= 384 && buf[0] == 'g' && buf[1] == 'E' && buf[2] == 'A';
}

static bool LooksLikeLanServerEntry(const char* buf, int len)
{
	if (!LooksLikeLanDiscoveryPacket(buf, len))
		return false;
	if ((unsigned char)buf[3] != 0x03)
		return false;
	if (std::memcmp(buf + 72, "9900|0", 6) != 0)
		return false;
	for (int i = 40; i < 72; ++i) {
		if (buf[i] != 0)
			return true;
	}
	return false;
}

static bool LooksLikeLanEmptyDiscovery(const char* buf, int len)
{
	if (!LooksLikeLanDiscoveryPacket(buf, len))
		return false;
	if ((unsigned char)buf[3] != 0x00)
		return false;
	if ((unsigned char)buf[8] != 0x3F)
		return false;
	for (int i = 9; i < 384; ++i) {
		if (buf[i] != 0)
			return false;
	}
	return true;
}

static bool ReplaceWithSyntheticLanDiscovery(SOCKET s, char* buf, int len, sockaddr* from, int* fromlen)
{
	if (LooksLikeLanEmptyDiscovery(buf, len)) {
		LogLanDiscoveryPacket("lan discovery replace input", buf, len, from, fromlen ? *fromlen : 0, "real-empty");
		if (FillSyntheticLanEmptyDiscovery(s, buf, len, from, fromlen))
			return true;
		return FillSyntheticLanDiscovery(s, buf, len, from, fromlen, true, true);
	}
	if (!LooksLikeLanServerEntry(buf, len) && !LooksLikeLanEmptyDiscovery(buf, len))
		return false;
	LogLanDiscoveryPacket("lan discovery replace input", buf, len, from, fromlen ? *fromlen : 0, "real-entry");
	QueueSyntheticLanCloneFromReal(s, buf, len, from, fromlen ? *fromlen : 0);
	return false;
}

static void MirrorLanDiscovery(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
	if (InterlockedCompareExchange(&gLanDiscoveryMirrorEnabled, 0, 0) == 0 ||
		InternalLanOwnsList() ||
		!gRealSendTo || !buf || len <= 0 || !IsBroadcastOrMulticast(to, tolen))
		return;

	const sockaddr_in* orig = reinterpret_cast<const sockaddr_in*>(to);
	sockaddr_in dst = {};
	if (gLanDiscovery.port != 0 && gLanDiscoveryAddr.sin_family == AF_INET) {
		dst = gLanDiscoveryAddr;
	} else if (!ResolveTo(gLan.host, ntohs(orig->sin_port), &dst)) {
		LogLine(
			"lan discovery mirror resolve failed broadcast=%s target_host=%s target_port=%u",
			AddrToString(to, tolen).c_str(),
			gLan.host,
			(unsigned)ntohs(orig->sin_port));
		return;
	}
	if (LooksLikeLanDiscoveryPacket(buf, len))
		LogLanDiscoveryPacket("lan discovery mirror input", buf, len, to, tolen, "broadcast-send");
	else if (LoggerVerboseData())
		LogDataPreview("lan discovery mirror non-gea input", buf, len);
	int rr = gRealSendTo(s, buf, len, flags, reinterpret_cast<const sockaddr*>(&dst), sizeof(dst));
	LogLine(
		"lan discovery mirror len=%d broadcast=%s target=%s rr=%d wsa=%d",
		len,
		AddrToString(to, tolen).c_str(),
		AddrToString(reinterpret_cast<sockaddr*>(&dst), sizeof(dst)).c_str(),
		rr,
		rr == SOCKET_ERROR ? WSAGetLastError() : 0);
}

static void TrimField(char* s)
{
	if (!s)
		return;
	char* p = s;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
		++p;
	if (p != s)
		std::memmove(s, p, std::strlen(p) + 1);
	size_t n = std::strlen(s);
	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
		s[--n] = 0;
}

static int CopyEndpointField(const char* buf, int len, const char* key, char* out, size_t outSize)
{
	if (!buf || len <= 0 || !key || !out || outSize == 0)
		return 0;
	out[0] = 0;
	size_t keyLen = std::strlen(key);
	for (int i = 0; i <= len - (int)keyLen; ++i) {
		if (_strnicmp(buf + i, key, keyLen) != 0)
			continue;
		int j = i + (int)keyLen;
		size_t k = 0;
		while (j < len && k + 1 < outSize) {
			char c = buf[j++];
			if (c == '\r' || c == '\n' || c == '\t' || c == '\0' || c == '|' || c == '&')
				break;
			out[k++] = c;
		}
		out[k] = 0;
		TrimField(out);
		return out[0] ? 1 : 0;
	}
	return 0;
}

static int CopyAnyEndpointField(const char* buf, int len, const char* const* keys, char* out, size_t outSize)
{
	if (!keys)
		return 0;
	for (int i = 0; keys[i]; ++i) {
		if (CopyEndpointField(buf, len, keys[i], out, outSize))
			return 1;
	}
	return 0;
}

static void SetRuntimeEndpointHost(Endpoint* ep, const char* host)
{
	if (!ep || !host || !host[0])
		return;
	lstrcpynA(ep->host, host, sizeof(ep->host));
}

static void SetRuntimeEndpointPort(Endpoint* ep, int port)
{
	if (!ep || port <= 0 || port > 65535)
		return;
	ep->port = static_cast<std::uint16_t>(port);
}

static void RefreshRuntimeEndpointAddrs()
{
	ResolveTo(gLan.host, gLan.port, &gLanAddr);
	ResolveTo(gRace.host, gRace.port, &gRaceAddr);
	ResolveTo(gLanControl.host, gLanControl.port, &gLanControlAddr);
	ResolveTo(gLanControlAlias.host, gLanControlAlias.port, &gLanControlAliasAddr);
	if (gLanDiscovery.port != 0)
		ResolveTo(gLanDiscovery.host, gLanDiscovery.port, &gLanDiscoveryAddr);
}

static void MaybeApplyAdvertisedEndpoint(
	const char* buf,
	int len,
	const char* tag,
	const char* service,
	Endpoint* ep,
	const char* const* hostKeys,
	const char* const* portKeys)
{
	char host[128] = {};
	char portTxt[32] = {};
	int gotHost = CopyAnyEndpointField(buf, len, hostKeys, host, sizeof(host));
	int gotPort = CopyAnyEndpointField(buf, len, portKeys, portTxt, sizeof(portTxt));
	if (!gotHost && !gotPort)
		return;

	int oldPort = ep ? ep->port : 0;
	char oldHost[128] = {};
	if (ep)
		lstrcpynA(oldHost, ep->host, sizeof(oldHost));

	if (host[0])
		SetRuntimeEndpointHost(ep, host);
	if (portTxt[0])
		SetRuntimeEndpointPort(ep, std::atoi(portTxt));
	RefreshRuntimeEndpointAddrs();

	if (ep && (_stricmp(oldHost, ep->host) != 0 || oldPort != (int)ep->port)) {
		LogLine("advertised %s applied from %s: %s:%u", service ? service : "endpoint", tag ? tag : "?", ep->host, (unsigned)ep->port);
	}
}

static void MaybeConsumeAdvertisedEndpoints(const char* buf, int len, const char* tag)
{
	static const char* const onlineHosts[] = {"BOOTSTRAPHOST=", "BOOTSTRAP_HOST=", "ONLINEHOST=", "ONLINE_HOST=", nullptr};
	static const char* const onlinePorts[] = {"BOOTSTRAPPORT=", "BOOTSTRAP_PORT=", "ONLINEPORT=", "ONLINE_PORT=", nullptr};
	static const char* const lobbyHosts[] = {"LOBBYHOST=", "LOBBY_HOST=", "LOBBYTCPHOST=", nullptr};
	static const char* const lobbyPorts[] = {"LOBBYTCP=", "LOBBYPORT=", "LOBBY_PORT=", "LOBBY_TCP_PORT=", nullptr};
	static const char* const controlHosts[] = {"CONTROLHOST=", "CONTROL_HOST=", "BUDDY_SERVER=", nullptr};
	static const char* const controlPorts[] = {"CONTROLPORT=", "CONTROL_PORT=", "BUDDY_PORT=", nullptr};
	static const char* const aliasHosts[] = {"CONTROLALIASHOST=", "CONTROL_ALIAS_HOST=", "CONTROLALIAS_HOST=", "BUDDY_ALIAS_SERVER=", nullptr};
	static const char* const aliasPorts[] = {"CONTROLALIASPORT=", "CONTROL_ALIAS_PORT=", "CONTROLALIAS_PORT=", "BUDDY_ALIAS_PORT=", nullptr};
	static const char* const raceHosts[] = {"UDPHOST=", "RLYHOST=", "RACEHOST=", "RACE_HOST=", nullptr};
	static const char* const racePorts[] = {"UDPPORT=", "RLYPORT=", "RACEPORT=", "RACE_PORT=", nullptr};

	MaybeApplyAdvertisedEndpoint(buf, len, tag, "online", &gOnline, onlineHosts, onlinePorts);
	MaybeApplyAdvertisedEndpoint(buf, len, tag, "lobby", &gLan, lobbyHosts, lobbyPorts);
	MaybeApplyAdvertisedEndpoint(buf, len, tag, "control", &gLanControl, controlHosts, controlPorts);
	MaybeApplyAdvertisedEndpoint(buf, len, tag, "control_alias", &gLanControlAlias, aliasHosts, aliasPorts);
	MaybeApplyAdvertisedEndpoint(buf, len, tag, "race", &gRace, raceHosts, racePorts);
}

static bool PatchIAT(HMODULE module, const char* importDll, const char* procName, void* replacement, void** originalOut)
{
	if (!module || !importDll || !procName || !replacement)
		return false;

	auto* base = reinterpret_cast<std::uint8_t*>(module);
	auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
	if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		return false;
	auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE)
		return false;
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress || !dir.Size)
		return false;

	auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
	for (; imp->Name; ++imp) {
		const char* dllName = reinterpret_cast<const char*>(base + imp->Name);
		if (_stricmp(dllName, importDll) != 0)
			continue;

		auto* thunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
		auto* origThunk = imp->OriginalFirstThunk
			? reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk)
			: thunk;

		for (; origThunk->u1.AddressOfData; ++origThunk, ++thunk) {
			if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal))
				continue;
			auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
			if (std::strcmp(reinterpret_cast<const char*>(ibn->Name), procName) != 0)
				continue;

			DWORD oldProtect = 0;
			if (!VirtualProtect(&thunk->u1.Function, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect))
				return false;
			if (originalOut && !*originalOut)
				*originalOut = reinterpret_cast<void*>(thunk->u1.Function);
#ifdef _WIN64
			thunk->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
#else
			thunk->u1.Function = reinterpret_cast<DWORD_PTR>(replacement);
#endif
			DWORD tmp = 0;
			VirtualProtect(&thunk->u1.Function, sizeof(void*), oldProtect, &tmp);
			FlushInstructionCache(GetCurrentProcess(), &thunk->u1.Function, sizeof(void*));
			return true;
		}
	}
	return false;
}

static bool IsWsModule(HMODULE mod)
{
	char path[MAX_PATH] = {};
	if (!GetModuleFileNameA(mod, path, sizeof(path)))
		return false;
	const char* file = std::strrchr(path, '\\');
	file = file ? file + 1 : path;
	return _stricmp(file, "ws2_32.dll") == 0 || _stricmp(file, "wsock32.dll") == 0;
}

static FARPROC WINAPI MyGetProcAddress(HMODULE mod, LPCSTR proc)
{
	if (!gRealGetProcAddress)
		return nullptr;
	if (proc && !IS_INTRESOURCE(proc) && IsWsModule(mod)) {
		if (std::strcmp(proc, "connect") == 0) return reinterpret_cast<FARPROC>(&MyConnect);
		if (std::strcmp(proc, "WSAConnect") == 0) return reinterpret_cast<FARPROC>(&MyWSAConnect);
		if (std::strcmp(proc, "send") == 0) return reinterpret_cast<FARPROC>(&MySend);
		if (std::strcmp(proc, "recv") == 0) return reinterpret_cast<FARPROC>(&MyRecv);
		if (std::strcmp(proc, "sendto") == 0) return reinterpret_cast<FARPROC>(&MySendTo);
		if (std::strcmp(proc, "recvfrom") == 0) return reinterpret_cast<FARPROC>(&MyRecvFrom);
		if (std::strcmp(proc, "WSASend") == 0) return reinterpret_cast<FARPROC>(&MyWSASend);
		if (std::strcmp(proc, "WSARecv") == 0) return reinterpret_cast<FARPROC>(&MyWSARecv);
		if (std::strcmp(proc, "WSASendTo") == 0) return reinterpret_cast<FARPROC>(&MyWSASendTo);
		if (std::strcmp(proc, "WSARecvFrom") == 0) return reinterpret_cast<FARPROC>(&MyWSARecvFrom);
		if (std::strcmp(proc, "bind") == 0) return reinterpret_cast<FARPROC>(&MyBind);
		if (std::strcmp(proc, "getsockname") == 0) return reinterpret_cast<FARPROC>(&MyGetSockName);
		if (std::strcmp(proc, "closesocket") == 0) return reinterpret_cast<FARPROC>(&MyCloseSocket);
		if (std::strcmp(proc, "socket") == 0) return reinterpret_cast<FARPROC>(&MySocket);
		if (std::strcmp(proc, "WSASocketA") == 0) return reinterpret_cast<FARPROC>(&MyWSASocketA);
	}
	return gRealGetProcAddress(mod, proc);
}

static void HookModule(HMODULE mod)
{
	if (!mod || mod == gSelf)
		return;
	PatchIAT(mod, "ws2_32.dll", "connect", reinterpret_cast<void*>(&MyConnect), reinterpret_cast<void**>(&gRealConnect));
	PatchIAT(mod, "wsock32.dll", "connect", reinterpret_cast<void*>(&MyConnect), reinterpret_cast<void**>(&gRealConnect));
	PatchIAT(mod, "ws2_32.dll", "WSAConnect", reinterpret_cast<void*>(&MyWSAConnect), reinterpret_cast<void**>(&gRealWSAConnect));
	PatchIAT(mod, "ws2_32.dll", "send", reinterpret_cast<void*>(&MySend), reinterpret_cast<void**>(&gRealSend));
	PatchIAT(mod, "wsock32.dll", "send", reinterpret_cast<void*>(&MySend), reinterpret_cast<void**>(&gRealSend));
	PatchIAT(mod, "ws2_32.dll", "recv", reinterpret_cast<void*>(&MyRecv), reinterpret_cast<void**>(&gRealRecv));
	PatchIAT(mod, "wsock32.dll", "recv", reinterpret_cast<void*>(&MyRecv), reinterpret_cast<void**>(&gRealRecv));
	PatchIAT(mod, "ws2_32.dll", "sendto", reinterpret_cast<void*>(&MySendTo), reinterpret_cast<void**>(&gRealSendTo));
	PatchIAT(mod, "wsock32.dll", "sendto", reinterpret_cast<void*>(&MySendTo), reinterpret_cast<void**>(&gRealSendTo));
	PatchIAT(mod, "ws2_32.dll", "recvfrom", reinterpret_cast<void*>(&MyRecvFrom), reinterpret_cast<void**>(&gRealRecvFrom));
	PatchIAT(mod, "wsock32.dll", "recvfrom", reinterpret_cast<void*>(&MyRecvFrom), reinterpret_cast<void**>(&gRealRecvFrom));
	PatchIAT(mod, "ws2_32.dll", "WSASend", reinterpret_cast<void*>(&MyWSASend), reinterpret_cast<void**>(&gRealWSASend));
	PatchIAT(mod, "ws2_32.dll", "WSARecv", reinterpret_cast<void*>(&MyWSARecv), reinterpret_cast<void**>(&gRealWSARecv));
	PatchIAT(mod, "ws2_32.dll", "WSASendTo", reinterpret_cast<void*>(&MyWSASendTo), reinterpret_cast<void**>(&gRealWSASendTo));
	PatchIAT(mod, "ws2_32.dll", "WSARecvFrom", reinterpret_cast<void*>(&MyWSARecvFrom), reinterpret_cast<void**>(&gRealWSARecvFrom));
	PatchIAT(mod, "ws2_32.dll", "bind", reinterpret_cast<void*>(&MyBind), reinterpret_cast<void**>(&gRealBind));
	PatchIAT(mod, "wsock32.dll", "bind", reinterpret_cast<void*>(&MyBind), reinterpret_cast<void**>(&gRealBind));
	PatchIAT(mod, "ws2_32.dll", "getsockname", reinterpret_cast<void*>(&MyGetSockName), reinterpret_cast<void**>(&gRealGetSockName));
	PatchIAT(mod, "wsock32.dll", "getsockname", reinterpret_cast<void*>(&MyGetSockName), reinterpret_cast<void**>(&gRealGetSockName));
	PatchIAT(mod, "ws2_32.dll", "getpeername", reinterpret_cast<void*>(&MyGetPeerName), reinterpret_cast<void**>(&gRealGetPeerName));
	PatchIAT(mod, "wsock32.dll", "getpeername", reinterpret_cast<void*>(&MyGetPeerName), reinterpret_cast<void**>(&gRealGetPeerName));
	PatchIAT(mod, "ws2_32.dll", "closesocket", reinterpret_cast<void*>(&MyCloseSocket), reinterpret_cast<void**>(&gRealCloseSocket));
	PatchIAT(mod, "wsock32.dll", "closesocket", reinterpret_cast<void*>(&MyCloseSocket), reinterpret_cast<void**>(&gRealCloseSocket));
	PatchIAT(mod, "ws2_32.dll", "socket", reinterpret_cast<void*>(&MySocket), reinterpret_cast<void**>(&gRealSocket));
	PatchIAT(mod, "wsock32.dll", "socket", reinterpret_cast<void*>(&MySocket), reinterpret_cast<void**>(&gRealSocket));
	PatchIAT(mod, "ws2_32.dll", "WSASocketA", reinterpret_cast<void*>(&MyWSASocketA), reinterpret_cast<void**>(&gRealWSASocketA));
	PatchIAT(mod, "kernel32.dll", "GetProcAddress", reinterpret_cast<void*>(&MyGetProcAddress), reinterpret_cast<void**>(&gRealGetProcAddress));
	PatchIAT(mod, "kernelbase.dll", "GetProcAddress", reinterpret_cast<void*>(&MyGetProcAddress), reinterpret_cast<void**>(&gRealGetProcAddress));
}

static void HookAllModules()
{
	DWORD pid = GetCurrentProcessId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
	if (snap == INVALID_HANDLE_VALUE) {
		HookModule(GetModuleHandleA(nullptr));
		return;
	}
	MODULEENTRY32 me{};
	me.dwSize = sizeof(me);
	if (Module32First(snap, &me)) {
		do {
			HookModule(reinterpret_cast<HMODULE>(me.hModule));
		} while (Module32Next(snap, &me));
	}
	CloseHandle(snap);
}

static DWORD WINAPI HookRefreshThread(LPVOID)
{
	for (;;) {
		Sleep(1000);
		HookAllModules();
	}
	return 0;
}

static SOCKET WSAAPI MySocket(int af, int type, int protocol)
{
	if (!gRealSocket)
		return INVALID_SOCKET;
	SOCKET s = gRealSocket(af, type, protocol);
	LogLine("socket af=%d type=%s(%d) proto=%d -> 0x%Ix wsa=%d", af, SockTypeName(type), type, protocol, static_cast<size_t>(s), s == INVALID_SOCKET ? WSAGetLastError() : 0);
	if (s != INVALID_SOCKET)
		SockMetaSet(s, af, type, protocol);
	return s;
}

static SOCKET WSAAPI MyWSASocketA(int af, int type, int protocol, LPWSAPROTOCOL_INFOA info, GROUP g, DWORD flags)
{
	if (!gRealWSASocketA)
		return INVALID_SOCKET;
	SOCKET s = gRealWSASocketA(af, type, protocol, info, g, flags);
	LogLine("WSASocketA af=%d type=%s(%d) proto=%d flags=0x%08lX -> 0x%Ix wsa=%d", af, SockTypeName(type), type, protocol, (unsigned long)flags, static_cast<size_t>(s), s == INVALID_SOCKET ? WSAGetLastError() : 0);
	if (s != INVALID_SOCKET)
		SockMetaSet(s, af, type, protocol);
	return s;
}

static int WSAAPI MyBind(SOCKET s, const sockaddr* name, int namelen)
{
	if (!gRealBind)
		return SOCKET_ERROR;
	SockMeta meta = SockMetaGet(s);
	std::string addr = AddrToString(name, namelen);
	int rr = gRealBind(s, name, namelen);
	LogLine("bind s=0x%Ix af=%d type=%s addr=%s rr=%d wsa=%d", static_cast<size_t>(s), meta.af, SockTypeName(meta.type), addr.c_str(), rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0);
	return rr;
}

static int WSAAPI MyConnect(SOCKET s, const sockaddr* name, int namelen)
{
	if (!gRealConnect)
		return SOCKET_ERROR;
	SockMeta meta = SockMetaGet(s);
	std::string addr = AddrToString(name, namelen);
	LogLine("connect s=0x%Ix af=%d type=%s dest=%s", static_cast<size_t>(s), meta.af, SockTypeName(meta.type), addr.c_str());
	sockaddr_in redirect = {};
	const char* epName = nullptr;
	const sockaddr* actualName = name;
	int actualLen = namelen;
	if (BuildLanTcpRedirect(name, namelen, &redirect, &epName)) {
		actualName = reinterpret_cast<const sockaddr*>(&redirect);
		actualLen = sizeof(redirect);
		LogLine("connect redirect %s %s -> %s", epName ? epName : "lan", addr.c_str(), AddrToString(actualName, actualLen).c_str());
	}
	int rr = gRealConnect(s, actualName, actualLen);
	LogLine("connect result s=0x%Ix rr=%d wsa=%d", static_cast<size_t>(s), rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0);
	if (rr == 0)
		LogSocketName("connect names", s);
	return rr;
}

static int WSAAPI MyWSAConnect(SOCKET s, const sockaddr* name, int namelen, LPWSABUF caller, LPWSABUF callee, LPQOS sqos, LPQOS gqos)
{
	if (!gRealWSAConnect)
		return SOCKET_ERROR;
	SockMeta meta = SockMetaGet(s);
	std::string addr = AddrToString(name, namelen);
	LogLine("WSAConnect s=0x%Ix af=%d type=%s dest=%s", static_cast<size_t>(s), meta.af, SockTypeName(meta.type), addr.c_str());
	sockaddr_in redirect = {};
	const char* epName = nullptr;
	const sockaddr* actualName = name;
	int actualLen = namelen;
	if (BuildLanTcpRedirect(name, namelen, &redirect, &epName)) {
		actualName = reinterpret_cast<const sockaddr*>(&redirect);
		actualLen = sizeof(redirect);
		LogLine("WSAConnect redirect %s %s -> %s", epName ? epName : "lan", addr.c_str(), AddrToString(actualName, actualLen).c_str());
	}
	int rr = gRealWSAConnect(s, actualName, actualLen, caller, callee, sqos, gqos);
	LogLine("WSAConnect result s=0x%Ix rr=%d wsa=%d", static_cast<size_t>(s), rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0);
	if (rr == 0)
		LogSocketName("WSAConnect names", s);
	return rr;
}

static int WSAAPI MySend(SOCKET s, const char* buf, int len, int flags)
{
	if (!gRealSend)
		return SOCKET_ERROR;
	int rr = gRealSend(s, buf, len, flags);
	LogTcpStreamData("send", s, buf, len, flags, rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0, 1, false);
	return rr;
}

static int WSAAPI MyRecv(SOCKET s, char* buf, int len, int flags)
{
	if (!gRealRecv)
		return SOCKET_ERROR;
	int rr = gRealRecv(s, buf, len, flags);
	if (rr > 0)
		MaybeConsumeAdvertisedEndpoints(buf, rr, "recv");
	LogTcpStreamData("recv", s, rr > 0 ? buf : nullptr, rr > 0 ? rr : len, flags, rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0, 1, false);
	return rr;
}

static int WSAAPI MySendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
	if (!gRealSendTo)
		return SOCKET_ERROR;
	SockMeta meta = SockMetaGet(s);
	std::string addr = AddrToString(to, tolen);
	LogLine("sendto s=0x%Ix type=%s len=%d flags=0x%X dest=%s", static_cast<size_t>(s), SockTypeName(meta.type), len, flags, addr.c_str());
	if (LooksLikeLanDiscoveryPacket(buf, len) && !IsBroadcastOrMulticast(to, tolen))
		LogLanDiscoveryPacket("sendto", buf, len, to, tolen, "direct-gea");
	MirrorLanDiscovery(s, buf, len, flags, to, tolen);
	sockaddr_in raceRedirect = {};
	const sockaddr* actualTo = to;
	int actualToLen = tolen;
	if (BuildRaceUdpRedirect(buf, len, to, tolen, &raceRedirect)) {
		actualTo = reinterpret_cast<const sockaddr*>(&raceRedirect);
		actualToLen = sizeof(raceRedirect);
		LogLine("sendto race redirect %s -> %s", addr.c_str(), AddrToString(actualTo, actualToLen).c_str());
	}
	int rr = gRealSendTo(s, buf, len, flags, actualTo, actualToLen);
	LogLine("sendto result s=0x%Ix rr=%d wsa=%d", static_cast<size_t>(s), rr, rr == SOCKET_ERROR ? WSAGetLastError() : 0);
	return rr;
}

static int WSAAPI MyRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
{
	if (!gRealRecvFrom)
		return SOCKET_ERROR;
	if (FillPendingSyntheticLanEntry(s, buf, len, from, fromlen))
		return 384;
	if (FillSyntheticLanDiscovery(s, buf, len, from, fromlen, false, false))
		return 384;
	int rr = gRealRecvFrom(s, buf, len, flags, from, fromlen);
	if (rr > 0) {
		if (ReplaceWithSyntheticLanDiscovery(s, buf, rr, from, fromlen))
			rr = 384;
		else if (LooksLikeLanDiscoveryPacket(buf, rr))
			LogLanDiscoveryPacket("recvfrom real", buf, rr, from, fromlen ? *fromlen : 0, "not-replaced");
	}
	std::string addr = (rr >= 0 && from && fromlen) ? AddrToString(from, *fromlen) : std::string("<unknown>");
	if (rr > 0 || LoggerVerboseData()) {
		LogLine("recvfrom s=0x%Ix rr=%d flags=0x%X from=%s wsa=%d", static_cast<size_t>(s), rr, flags, addr.c_str(), rr == SOCKET_ERROR ? WSAGetLastError() : 0);
		if (rr > 0)
			LogDataPreview("recvfrom", buf, rr);
	}
	return rr;
}

static int WSAAPI MyWSASend(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
	if (!gRealWSASend)
		return SOCKET_ERROR;
	DWORD total = 0;
	for (DWORD i = 0; bufs && i < count; ++i)
		total += bufs[i].len;
	int rr = gRealWSASend(s, bufs, count, sent, flags, ov, cb);
	int shown = 0;
	const char* shownBuf = nullptr;
	if (bufs && count > 0 && bufs[0].buf && bufs[0].len > 0) {
		shownBuf = bufs[0].buf;
		shown = (int)bufs[0].len;
	}
	LogTcpStreamData(
		"WSASend",
		s,
		shownBuf,
		shown,
		(int)flags,
		rr,
		rr == SOCKET_ERROR ? WSAGetLastError() : 0,
		count,
		ov != nullptr);
	if (count > 1)
		LogLine("WSASend buffers s=0x%Ix count=%lu total=%lu sent=%lu", static_cast<size_t>(s), (unsigned long)count, (unsigned long)total, sent ? (unsigned long)*sent : 0UL);
	return rr;
}

static int WSAAPI MyWSARecv(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD recvd, LPDWORD flags, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
	if (!gRealWSARecv)
		return SOCKET_ERROR;
	int rr = gRealWSARecv(s, bufs, count, recvd, flags, ov, cb);
	DWORD got = (rr == 0 && recvd) ? *recvd : 0;
	const char* shownBuf = nullptr;
	int shown = 0;
	if (got > 0 && bufs && count > 0 && bufs[0].buf) {
		shownBuf = bufs[0].buf;
		shown = (int)got;
		if (shown > (int)bufs[0].len)
			shown = (int)bufs[0].len;
		MaybeConsumeAdvertisedEndpoints(shownBuf, shown, "WSARecv");
	}
	LogTcpStreamData(
		"WSARecv",
		s,
		shownBuf,
		shown,
		flags ? (int)*flags : 0,
		rr,
		rr == SOCKET_ERROR ? WSAGetLastError() : 0,
		count,
		ov != nullptr);
	return rr;
}

static int WSAAPI MyWSASendTo(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD sent, DWORD flags, const sockaddr* to, int tolen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
	if (!gRealWSASendTo)
		return SOCKET_ERROR;
	DWORD total = 0;
	for (DWORD i = 0; i < count; ++i)
		total += bufs[i].len;
	SockMeta meta = SockMetaGet(s);
	std::string addr = AddrToString(to, tolen);
	LogLine("WSASendTo s=0x%Ix type=%s count=%lu total=%lu flags=0x%lX dest=%s", static_cast<size_t>(s), SockTypeName(meta.type), (unsigned long)count, (unsigned long)total, (unsigned long)flags, addr.c_str());
	if (count == 1 && bufs && bufs[0].buf && bufs[0].len > 0 && bufs[0].len <= 65535) {
		if (LooksLikeLanDiscoveryPacket(bufs[0].buf, (int)bufs[0].len) && !IsBroadcastOrMulticast(to, tolen))
			LogLanDiscoveryPacket("WSASendTo", bufs[0].buf, (int)bufs[0].len, to, tolen, "direct-gea");
		MirrorLanDiscovery(s, bufs[0].buf, (int)bufs[0].len, (int)flags, to, tolen);
	}
	sockaddr_in raceRedirect = {};
	const sockaddr* actualTo = to;
	int actualToLen = tolen;
	if (count == 1 && bufs && bufs[0].buf && bufs[0].len > 0 && bufs[0].len <= 65535 &&
		BuildRaceUdpRedirect(bufs[0].buf, (int)bufs[0].len, to, tolen, &raceRedirect)) {
		actualTo = reinterpret_cast<const sockaddr*>(&raceRedirect);
		actualToLen = sizeof(raceRedirect);
		LogLine("WSASendTo race redirect %s -> %s", addr.c_str(), AddrToString(actualTo, actualToLen).c_str());
	}
	int rr = gRealWSASendTo(s, bufs, count, sent, flags, actualTo, actualToLen, ov, cb);
	LogLine("WSASendTo result s=0x%Ix rr=%d sent=%lu wsa=%d", static_cast<size_t>(s), rr, sent ? (unsigned long)*sent : 0UL, rr == SOCKET_ERROR ? WSAGetLastError() : 0);
	return rr;
}

static int WSAAPI MyWSARecvFrom(SOCKET s, LPWSABUF bufs, DWORD count, LPDWORD recvd, LPDWORD flags, sockaddr* from, LPINT fromlen, LPWSAOVERLAPPED ov, LPWSAOVERLAPPED_COMPLETION_ROUTINE cb)
{
	if (!gRealWSARecvFrom)
		return SOCKET_ERROR;
	if (!ov && bufs && count > 0 && bufs[0].buf &&
		FillPendingSyntheticLanEntry(s, bufs[0].buf, (int)bufs[0].len, from, fromlen)) {
		if (recvd)
			*recvd = 384;
		if (flags)
			*flags = 0;
		return 0;
	}
	if (!ov && bufs && count > 0 && bufs[0].buf &&
		FillSyntheticLanDiscovery(s, bufs[0].buf, (int)bufs[0].len, from, fromlen, false, false)) {
		if (recvd)
			*recvd = 384;
		if (flags)
			*flags = 0;
		return 0;
	}
	int rr = gRealWSARecvFrom(s, bufs, count, recvd, flags, from, fromlen, ov, cb);
	if (rr == 0 && recvd && *recvd > 0 && bufs && count > 0 && bufs[0].buf &&
		ReplaceWithSyntheticLanDiscovery(s, bufs[0].buf, (int)*recvd, from, fromlen)) {
		*recvd = 384;
		if (flags)
			*flags = 0;
	} else if (rr == 0 && recvd && *recvd > 0 && bufs && count > 0 && bufs[0].buf &&
		LooksLikeLanDiscoveryPacket(bufs[0].buf, (int)*recvd)) {
		LogLanDiscoveryPacket("WSARecvFrom real", bufs[0].buf, (int)*recvd, from, fromlen ? *fromlen : 0, "not-replaced");
	}
	std::string addr = (rr == 0 && from && fromlen) ? AddrToString(from, *fromlen) : std::string("<unknown>");
	if ((rr == 0 && recvd && *recvd > 0) || LoggerVerboseData()) {
		LogLine("WSARecvFrom s=0x%Ix rr=%d recvd=%lu flags=0x%lX from=%s wsa=%d", static_cast<size_t>(s), rr, recvd ? (unsigned long)*recvd : 0UL, flags ? (unsigned long)*flags : 0UL, addr.c_str(), rr == SOCKET_ERROR ? WSAGetLastError() : 0);
		if (rr == 0 && recvd && *recvd > 0 && bufs && count > 0 && bufs[0].buf) {
			LogDataPreview("WSARecvFrom", bufs[0].buf, (int)*recvd);
			if ((int)*recvd == 384 && LooksLikeLanDiscoveryPacket(bufs[0].buf, (int)*recvd))
				LogDataFullHex("WSARecvFrom", bufs[0].buf, (int)*recvd);
		}
	}
	return rr;
}

static int WSAAPI MyGetSockName(SOCKET s, sockaddr* name, int* namelen)
{
	if (!gRealGetSockName)
		return SOCKET_ERROR;
	int rr = gRealGetSockName(s, name, namelen);
	if (rr == 0 && name && namelen)
		LogLine("getsockname s=0x%Ix -> %s", static_cast<size_t>(s), AddrToString(name, *namelen).c_str());
	return rr;
}

static int WSAAPI MyGetPeerName(SOCKET s, sockaddr* name, int* namelen)
{
	if (!gRealGetPeerName)
		return SOCKET_ERROR;
	int rr = gRealGetPeerName(s, name, namelen);
	if (rr == 0 && name && namelen)
		LogLine("getpeername s=0x%Ix -> %s", static_cast<size_t>(s), AddrToString(name, *namelen).c_str());
	return rr;
}

static int WSAAPI MyCloseSocket(SOCKET s)
{
	if (!gRealCloseSocket)
		return SOCKET_ERROR;
	LogSocketName("closesocket names", s);
	SockMeta meta = SockMetaGet(s);
	LogLine("closesocket s=0x%Ix af=%d type=%s", static_cast<size_t>(s), meta.af, SockTypeName(meta.type));
	SockMetaClear(s);
	SyntheticLanClear(s);
	return gRealCloseSocket(s);
}

static void InitWinsockState()
{
	if (!gSockMetaLockReady) {
		InitializeCriticalSection(&gSockMetaLock);
		gSockMetaLockReady = true;
		for (auto& m : gSockMeta)
			m.s = INVALID_SOCKET;
	}
	if (!gSyntheticLanLockReady) {
		InitializeCriticalSection(&gSyntheticLanLock);
		gSyntheticLanLockReady = true;
		for (auto& m : gSyntheticLan)
			m.s = INVALID_SOCKET;
	}
}

void InitWinsockHooks(HMODULE selfModule)
{
	gSelf = selfModule;
	InitWinsockState();

	HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
	if (!ws2)
		ws2 = LoadLibraryA("ws2_32.dll");
	if (ws2) {
		if (!gRealConnect) gRealConnect = reinterpret_cast<ConnectFn>(GetProcAddress(ws2, "connect"));
		if (!gRealWSAConnect) gRealWSAConnect = reinterpret_cast<WSAConnectFn>(GetProcAddress(ws2, "WSAConnect"));
		if (!gRealSendTo) gRealSendTo = reinterpret_cast<SendToFn>(GetProcAddress(ws2, "sendto"));
		if (!gRealRecvFrom) gRealRecvFrom = reinterpret_cast<RecvFromFn>(GetProcAddress(ws2, "recvfrom"));
		if (!gRealWSASendTo) gRealWSASendTo = reinterpret_cast<WSASendToFn>(GetProcAddress(ws2, "WSASendTo"));
		if (!gRealWSARecvFrom) gRealWSARecvFrom = reinterpret_cast<WSARecvFromFn>(GetProcAddress(ws2, "WSARecvFrom"));
		if (!gRealBind) gRealBind = reinterpret_cast<BindFn>(GetProcAddress(ws2, "bind"));
		if (!gRealGetSockName) gRealGetSockName = reinterpret_cast<GetSockNameFn>(GetProcAddress(ws2, "getsockname"));
		if (!gRealGetPeerName) gRealGetPeerName = reinterpret_cast<GetPeerNameFn>(GetProcAddress(ws2, "getpeername"));
		if (!gRealCloseSocket) gRealCloseSocket = reinterpret_cast<CloseSocketFn>(GetProcAddress(ws2, "closesocket"));
		if (!gRealSocket) gRealSocket = reinterpret_cast<SocketFn>(GetProcAddress(ws2, "socket"));
		if (!gRealWSASocketA) gRealWSASocketA = reinterpret_cast<WSASocketAFn>(GetProcAddress(ws2, "WSASocketA"));
	}
	if (!gRealGetProcAddress)
		gRealGetProcAddress = reinterpret_cast<GetProcAddressFn>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress"));

	HookAllModules();
	HANDLE hookThread = CreateThread(nullptr, 0, HookRefreshThread, nullptr, 0, nullptr);
	if (hookThread)
		CloseHandle(hookThread);
	LogLine("winsock hooks installed");
}

void ShutdownWinsockHooks()
{
	if (gSockMetaLockReady) {
		DeleteCriticalSection(&gSockMetaLock);
		gSockMetaLockReady = false;
	}
	if (gSyntheticLanLockReady) {
		DeleteCriticalSection(&gSyntheticLanLock);
		gSyntheticLanLockReady = false;
	}
}
