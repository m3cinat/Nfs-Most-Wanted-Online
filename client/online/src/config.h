#pragma once

#include <winsock2.h>
#include <windows.h>
#include <cstdint>

struct Endpoint {
	char host[128];
	std::uint16_t port;
};

extern Endpoint gLan;
extern Endpoint gOnline;
extern Endpoint gRace;
extern Endpoint gLanControl;
extern Endpoint gLanControlAlias;
extern Endpoint gLanDiscovery;
extern char gLanDiscoveryHostId[32];
extern char gLanDiscoveryName[32];
extern char gLanInternalFakeName[32];
extern std::uint16_t gLanInternalFakeShort;
extern std::uint32_t gLanInternalFakeId;
extern sockaddr_in gLanAddr;
extern sockaddr_in gRaceAddr;
extern sockaddr_in gLanControlAddr;
extern sockaddr_in gLanControlAliasAddr;
extern sockaddr_in gLanDiscoveryAddr;
extern volatile LONG gLanOverrideEnabled;
extern volatile LONG gOnlineOverrideEnabled;
extern volatile LONG gCertPatchEnabled;
extern volatile LONG gLanDiscoveryMirrorEnabled;
extern volatile LONG gLanDiscoveryInjectEnabled;
extern volatile LONG gLanInternalHooksEnabled;
extern volatile LONG gLanInternalFakeEnabled;
extern volatile LONG gLanInternalEventHookEnabled;
extern volatile LONG gLanInternalRefreshStateEnabled;
extern volatile LONG gLanInternalSelectedHostHookEnabled;
extern volatile LONG gLanInternalForceOpenSelectedEnabled;

void LoadConfig(HMODULE selfModule);
bool ResolveTo(const char* host, std::uint16_t port, sockaddr_in* out);
int ParseBoolLike(const char* v);
bool NameEq(const char* a, const char* b);
