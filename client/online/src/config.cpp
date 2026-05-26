#include "config.h"

#include "logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

Endpoint gLan = {"127.0.0.1", 9900};
Endpoint gOnline = {"127.0.0.1", 30920};
Endpoint gRace = {"127.0.0.1", 2000};
Endpoint gLanControl = {"127.0.0.1", 20923};
Endpoint gLanControlAlias = {"127.0.0.1", 13505};
Endpoint gLanDiscovery = {"127.0.0.1", 0};
char gLanDiscoveryHostId[32] = "NFSMWNA";
char gLanDiscoveryName[32] = "MWONLINE";
char gLanInternalFakeName[32] = "MWONLINE";
std::uint16_t gLanInternalFakeShort = 9900;
std::uint32_t gLanInternalFakeId = 1;
sockaddr_in gLanAddr = {};
sockaddr_in gRaceAddr = {};
sockaddr_in gLanControlAddr = {};
sockaddr_in gLanControlAliasAddr = {};
sockaddr_in gLanDiscoveryAddr = {};
volatile LONG gLanOverrideEnabled = 0;
volatile LONG gOnlineOverrideEnabled = 0;
volatile LONG gCertPatchEnabled = 1;
volatile LONG gLanDiscoveryMirrorEnabled = 1;
volatile LONG gLanDiscoveryInjectEnabled = 1;
volatile LONG gLanInternalHooksEnabled = 1;
volatile LONG gLanInternalFakeEnabled = 1;
volatile LONG gLanInternalEventHookEnabled = 0;
volatile LONG gLanInternalRefreshStateEnabled = 0;
volatile LONG gLanInternalSelectedHostHookEnabled = 1;
volatile LONG gLanInternalForceOpenSelectedEnabled = 0;

static void Trim(char* s)
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

bool NameEq(const char* a, const char* b)
{
	return a && b && _stricmp(a, b) == 0;
}

int ParseBoolLike(const char* v)
{
	if (!v || !v[0])
		return -1;
	if (NameEq(v, "1") || NameEq(v, "on") || NameEq(v, "true") || NameEq(v, "yes"))
		return 1;
	if (NameEq(v, "0") || NameEq(v, "off") || NameEq(v, "false") || NameEq(v, "no"))
		return 0;
	return -1;
}

static void SetEndpointHost(Endpoint* ep, const char* host)
{
	if (!ep || !host || !host[0])
		return;
	lstrcpynA(ep->host, host, sizeof(ep->host));
}

static void SetEndpointPort(Endpoint* ep, int port)
{
	if (!ep || port <= 0 || port > 65535)
		return;
	ep->port = (std::uint16_t)port;
}

bool ResolveTo(const char* host, std::uint16_t port, sockaddr_in* out)
{
	if (!host || !*host || !out || port == 0)
		return false;
	std::memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port = htons(port);
	unsigned long direct = inet_addr(host);
	if (direct != INADDR_NONE) {
		out->sin_addr.s_addr = direct;
		return true;
	}
	hostent* he = gethostbyname(host);
	if (!he || !he->h_addr_list || !he->h_addr_list[0])
		return false;
	std::memcpy(&out->sin_addr, he->h_addr_list[0], 4);
	return true;
}

static void RefreshAddrs()
{
	ResolveTo(gLan.host, gLan.port, &gLanAddr);
	ResolveTo(gRace.host, gRace.port, &gRaceAddr);
	ResolveTo(gLanControl.host, gLanControl.port, &gLanControlAddr);
	ResolveTo(gLanControlAlias.host, gLanControlAlias.port, &gLanControlAliasAddr);
	if (gLanDiscovery.port != 0)
		ResolveTo(gLanDiscovery.host, gLanDiscovery.port, &gLanDiscoveryAddr);
}

static void LoadConfigPath(const char* path)
{
	FILE* f = std::fopen(path, "rb");
	if (!f)
		return;

	char serverHost[128] = {};
	char onlineHost[128] = {};
	char raceHost[128] = {};
	char lanHost[128] = {};
	char lanControlHost[128] = {};
	char lanControlAliasHost[128] = {};
	char lanDiscoveryHost[128] = {};
	int onlinePort = 0;
	int racePort = 0;
	int lanPort = 0;
	int lanControlPort = 0;
	int lanControlAliasPort = 0;
	int lanDiscoveryPort = 0;

	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		char* comment = std::strchr(line, '#');
		if (comment) *comment = 0;
		char* eq = std::strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		char key[128];
		char val[256];
		lstrcpynA(key, line, sizeof(key));
		lstrcpynA(val, eq + 1, sizeof(val));
		Trim(key);
		Trim(val);
		if (!key[0] || !val[0]) continue;

		if (NameEq(key, "server_host") || NameEq(key, "main_host") || NameEq(key, "host") || NameEq(key, "online_host")) {
			lstrcpynA(serverHost, val, sizeof(serverHost));
			if (NameEq(key, "online_host"))
				lstrcpynA(onlineHost, val, sizeof(onlineHost));
		} else if (NameEq(key, "bootstrap_host") || NameEq(key, "lobby_online_host") || NameEq(key, "online_lobby_host")) {
			lstrcpynA(onlineHost, val, sizeof(onlineHost));
		} else if (NameEq(key, "race_host") || NameEq(key, "race_udp_host")) {
			lstrcpynA(raceHost, val, sizeof(raceHost));
		} else if (NameEq(key, "lan_host") || NameEq(key, "lobby_host") || NameEq(key, "lobby_tcp_host")) {
			lstrcpynA(lanHost, val, sizeof(lanHost));
		} else if (NameEq(key, "control_host") || NameEq(key, "buddy_host")) {
			lstrcpynA(lanControlHost, val, sizeof(lanControlHost));
		} else if (NameEq(key, "control_alias_host") || NameEq(key, "buddy_alias_host")) {
			lstrcpynA(lanControlAliasHost, val, sizeof(lanControlAliasHost));
		} else if (NameEq(key, "lan_discovery_host") || NameEq(key, "discovery_host")) {
			lstrcpynA(lanDiscoveryHost, val, sizeof(lanDiscoveryHost));
		} else if (NameEq(key, "lan_discovery_host_id") || NameEq(key, "lan_host_id")) {
			lstrcpynA(gLanDiscoveryHostId, val, sizeof(gLanDiscoveryHostId));
		} else if (NameEq(key, "lan_discovery_name") || NameEq(key, "lan_server_name")) {
			lstrcpynA(gLanDiscoveryName, val, sizeof(gLanDiscoveryName));
		} else if (NameEq(key, "lan_internal_fake_name") || NameEq(key, "lan_fake_name")) {
			lstrcpynA(gLanInternalFakeName, val, sizeof(gLanInternalFakeName));
		} else if (NameEq(key, "lan_internal_fake_short") || NameEq(key, "lan_fake_short")) {
			int v = std::atoi(val);
			if (v > 0 && v <= 65535)
				gLanInternalFakeShort = (std::uint16_t)v;
		} else if (NameEq(key, "lan_internal_fake_id") || NameEq(key, "lan_fake_id")) {
			gLanInternalFakeId = (std::uint32_t)std::strtoul(val, nullptr, 0);
		} else if (NameEq(key, "lan_port") || NameEq(key, "lobby_port") || NameEq(key, "lobby_tcp_port")) {
			lanPort = std::atoi(val);
		} else if (NameEq(key, "bootstrap_port") || NameEq(key, "online_port") || NameEq(key, "online_lobby_port") || NameEq(key, "lobby_online_port")) {
			onlinePort = std::atoi(val);
		} else if (NameEq(key, "race_port") || NameEq(key, "race_udp_port")) {
			racePort = std::atoi(val);
		} else if (NameEq(key, "lan_control_port") || NameEq(key, "control_port") || NameEq(key, "buddy_port")) {
			lanControlPort = std::atoi(val);
		} else if (NameEq(key, "lan_control_alias_port") || NameEq(key, "control_alias_port") || NameEq(key, "buddy_alias_port")) {
			lanControlAliasPort = std::atoi(val);
		} else if (NameEq(key, "lan_discovery_port") || NameEq(key, "discovery_port")) {
			lanDiscoveryPort = std::atoi(val);
		} else if (NameEq(key, "lan_override_host") || NameEq(key, "lan_override")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanOverrideEnabled, b ? 1 : 0);
		} else if (NameEq(key, "online_override_host") || NameEq(key, "online_override")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gOnlineOverrideEnabled, b ? 1 : 0);
		} else if (NameEq(key, "cert_patch") || NameEq(key, "ssl_patch")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gCertPatchEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_provider_seed") || NameEq(key, "lan_discovery_mirror") || NameEq(key, "lan_inject") || NameEq(key, "lan_server_inject")) {
			int b = ParseBoolLike(val);
			if (b != -1) {
				InterlockedExchange(&gLanDiscoveryMirrorEnabled, b ? 1 : 0);
				InterlockedExchange(&gLanDiscoveryInjectEnabled, b ? 1 : 0);
			}
		} else if (NameEq(key, "lan_discovery_inject")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanDiscoveryInjectEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_hooks")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalHooksEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_event_hook") || NameEq(key, "lan_event_hook")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalEventHookEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_refresh_state") || NameEq(key, "lan_fake_refresh_state")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalRefreshStateEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_selected_host_hook") || NameEq(key, "lan_selected_host_hook")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalSelectedHostHookEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_force_open_selected") || NameEq(key, "lan_force_open_selected")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalForceOpenSelectedEnabled, b ? 1 : 0);
		} else if (NameEq(key, "lan_internal_fake") || NameEq(key, "lan_internal_fake_entry")) {
			int b = ParseBoolLike(val);
			if (b != -1)
				InterlockedExchange(&gLanInternalFakeEnabled, b ? 1 : 0);
		} else if (NameEq(key, "debug")) {
			LoggerSetVerboseData(ParseBoolLike(val) == 1);
		}
	}
	std::fclose(f);

	if (serverHost[0]) {
		SetEndpointHost(&gOnline, serverHost);
		SetEndpointHost(&gRace, serverHost);
		SetEndpointHost(&gLan, serverHost);
		SetEndpointHost(&gLanControl, serverHost);
		SetEndpointHost(&gLanControlAlias, serverHost);
		SetEndpointHost(&gLanDiscovery, serverHost);
	}
	if (onlineHost[0])
		SetEndpointHost(&gOnline, onlineHost);
	if (raceHost[0])
		SetEndpointHost(&gRace, raceHost);
	if (lanHost[0]) {
		SetEndpointHost(&gLan, lanHost);
		SetEndpointHost(&gLanControl, lanHost);
		SetEndpointHost(&gLanControlAlias, lanHost);
		SetEndpointHost(&gLanDiscovery, lanHost);
	}
	if (lanControlHost[0])
		SetEndpointHost(&gLanControl, lanControlHost);
	if (lanControlAliasHost[0])
		SetEndpointHost(&gLanControlAlias, lanControlAliasHost);
	if (lanDiscoveryHost[0])
		SetEndpointHost(&gLanDiscovery, lanDiscoveryHost);
	SetEndpointPort(&gOnline, onlinePort);
	SetEndpointPort(&gRace, racePort);
	SetEndpointPort(&gLan, lanPort);
	SetEndpointPort(&gLanControl, lanControlPort);
	SetEndpointPort(&gLanControlAlias, lanControlAliasPort);
	SetEndpointPort(&gLanDiscovery, lanDiscoveryPort);
}

void LoadConfig(HMODULE selfModule)
{
	char exe[MAX_PATH] = {};
	if (GetModuleFileNameA(nullptr, exe, sizeof(exe))) {
		char* slash = std::strrchr(exe, '\\');
		if (slash) {
			*(slash + 1) = 0;
			char cfg[MAX_PATH] = {};
			lstrcpynA(cfg, exe, sizeof(cfg));
			lstrcatA(cfg, "ONLINE.cfg");
			LoadConfigPath(cfg);
		}
	}

	char asi[MAX_PATH] = {};
	if (selfModule && GetModuleFileNameA(selfModule, asi, sizeof(asi))) {
		char* slash = std::strrchr(asi, '\\');
		if (slash) {
			*(slash + 1) = 0;
			char cfg[MAX_PATH] = {};
			lstrcpynA(cfg, asi, sizeof(cfg));
			lstrcatA(cfg, "ONLINE.cfg");
			LoadConfigPath(cfg);
		}
	}
	RefreshAddrs();
}
