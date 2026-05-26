#include "lan_select.h"

#include "config.h"
#include "logger.h"
#include "patterns.h"

#include <windows.h>
#include <cstdio>
#include <cstring>

#pragma pack(push, 1)
struct LanServerNode {
	LanServerNode* next;
	LanServerNode* prev;
	char name[13];
	char pad15[3];
	std::uint32_t metaPtr;
	std::uint16_t shortValue;
	std::uint16_t pad1e;
	std::uint32_t intValue;
};
#pragma pack(pop)

using GameMallocFn = void* (__cdecl*)(std::size_t);

static std::uint32_t ReadU32(void* self, std::size_t off)
{
	return *reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + off);
}

static void WriteU32(void* self, std::size_t off, std::uint32_t value)
{
	*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::uint8_t*>(self) + off) = value;
}

static LanServerNode* ListHead(void* self)
{
	return reinterpret_cast<LanServerNode*>(reinterpret_cast<std::uint8_t*>(self) + 0x8f0);
}

static void CopyNodeName(char* out, std::size_t outSize, const LanServerNode* node)
{
	if (!out || outSize == 0)
		return;
	out[0] = 0;
	if (!node)
		return;
	for (std::size_t i = 0; i + 1 < outSize && i < 13; ++i) {
		unsigned char c = (unsigned char)node->name[i];
		if (c == 0)
			break;
		out[i] = (c >= 32 && c <= 126) ? (char)c : '.';
		out[i + 1] = 0;
	}
}

static void CopyAsciiPreview(char* out, std::size_t outSize, const char* src, std::size_t maxLen)
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

static int CountLanServerList(void* self)
{
	if (!self)
		return 0;
	LanServerNode* head = ListHead(self);
	LanServerNode* node = head->next;
	int count = 0;
	while (node && node != head && count < 128) {
		++count;
		node = node->next;
	}
	return count;
}

void DumpLanSelectCore(const char* tag, void* self)
{
	if (!self) {
		LogLine("%s LANServerSelect this=<null>", tag ? tag : "lan-select");
		return;
	}
	char sourceName[80] = {};
	CopyAsciiPreview(sourceName, sizeof(sourceName), reinterpret_cast<char*>(reinterpret_cast<std::uint8_t*>(self) + 0x900), 64);
	LogLine(
		"%s LANServerSelect this=%p e4=0x%08lX 8ec=0x%08lX 8f0.next=0x%08lX 8f4.prev=0x%08lX 8f8=0x%08lX 8fc=0x%08lX 900='%s'",
		tag ? tag : "lan-select",
		self,
		(unsigned long)ReadU32(self, 0x0e4),
		(unsigned long)ReadU32(self, 0x8ec),
		(unsigned long)ReadU32(self, 0x8f0),
		(unsigned long)ReadU32(self, 0x8f4),
		(unsigned long)ReadU32(self, 0x8f8),
		(unsigned long)ReadU32(self, 0x8fc),
		sourceName[0] ? sourceName : "-");
}

int DumpLanServerList(const char* tag, void* self)
{
	if (!self) {
		LogLine("%s lan-list this=<null>", tag ? tag : "lan-list");
		return 0;
	}

	LanServerNode* head = ListHead(self);
	LanServerNode* node = head->next;
	int count = 0;
	LogLine("%s lan-list head=%p next=%p prev=%p", tag ? tag : "lan-list", head, head->next, head->prev);
	while (node && node != head && count < 128) {
		char name[32] = {};
		CopyNodeName(name, sizeof(name), node);
		LogLine(
			"%s lan-list node[%d]=%p next=%p prev=%p name='%s' ptr18=0x%08lX short1c=%u int20=0x%08lX",
			tag ? tag : "lan-list",
			count,
			node,
			node->next,
			node->prev,
			name[0] ? name : "-",
			(unsigned long)node->metaPtr,
			(unsigned)node->shortValue,
			(unsigned long)node->intValue);
		++count;
		node = node->next;
	}
	if (count >= 128)
		LogLine("%s lan-list stopped at guard=128", tag ? tag : "lan-list");
	LogLine("%s lan-list count=%d stored_8fc=%lu", tag ? tag : "lan-list", count, (unsigned long)ReadU32(self, 0x8fc));
	return count;
}

static bool ContainsLanEntryName(void* self, const char* wanted)
{
	if (!self || !wanted || !wanted[0])
		return false;
	LanServerNode* head = ListHead(self);
	LanServerNode* node = head->next;
	int guard = 0;
	while (node && node != head && guard++ < 128) {
		char name[32] = {};
		CopyNodeName(name, sizeof(name), node);
		if (_stricmp(name, wanted) == 0)
			return true;
		node = node->next;
	}
	return false;
}

bool InjectFakeLanServerEntry(void* self)
{
	if (!self || InterlockedCompareExchange(&gLanInternalFakeEnabled, 0, 0) == 0)
		return false;
	if (ContainsLanEntryName(self, gLanInternalFakeName)) {
		LogLine("lan-select fake entry already present name='%s'", gLanInternalFakeName);
		return false;
	}

	auto gameMalloc = reinterpret_cast<GameMallocFn>(SpeedVa(kGameMalloc));
	LanServerNode* node = reinterpret_cast<LanServerNode*>(gameMalloc(sizeof(LanServerNode)));
	if (!node) {
		LogLine("lan-select fake inject failed malloc size=0x%X", (unsigned)sizeof(LanServerNode));
		return false;
	}
	sockaddr_in selectedAddr = {};
	if (!ResolveTo(gLan.host, gLan.port ? gLan.port : 9900, &selectedAddr)) {
		LogLine("lan-select fake inject failed resolve host=%s port=%u", gLan.host, (unsigned)gLan.port);
		return false;
	}
	std::memset(node, 0, sizeof(*node));
	lstrcpynA(node->name, gLanInternalFakeName[0] ? gLanInternalFakeName : "MWONLINE", sizeof(node->name));
	node->metaPtr = ntohl(selectedAddr.sin_addr.s_addr);
	node->shortValue = gLanInternalFakeShort;
	node->intValue = gLanInternalFakeId;

	LanServerNode* head = ListHead(self);
	LanServerNode* tail = head->prev ? head->prev : head;
	tail->next = node;
	head->prev = node;
	node->prev = tail;
	node->next = head;

	int count = CountLanServerList(self);
	WriteU32(self, 0x8f8, (std::uint32_t)count);
	WriteU32(self, 0x8fc, (std::uint32_t)count);

	std::uint32_t oldTop = ReadU32(self, 0x33c);
	std::uint32_t oldSelected = ReadU32(self, 0x340);
	std::uint32_t oldUiCount = ReadU32(self, 0x348);
	std::uint32_t oldJoinState = ReadU32(self, 0x34c);
	if (count > 0) {
		WriteU32(self, 0x33c, 1);
		WriteU32(self, 0x340, 1);
		WriteU32(self, 0x348, (std::uint32_t)count);
		WriteU32(self, 0x34c, 0);
	}
	LogLine(
		"lan-select fake injected node=%p name='%s' host=%s ptr18_ip=0x%08lX net_ip=0x%08lX short1c=%u int20=0x%08lX count=%d ui top=%lu->%lu selected=%lu->%lu ui_count=%lu->%lu join_state=%lu->%lu",
		node,
		node->name,
		gLan.host,
		(unsigned long)node->metaPtr,
		(unsigned long)selectedAddr.sin_addr.s_addr,
		(unsigned)node->shortValue,
		(unsigned long)node->intValue,
		count,
		(unsigned long)oldTop,
		(unsigned long)ReadU32(self, 0x33c),
		(unsigned long)oldSelected,
		(unsigned long)ReadU32(self, 0x340),
		(unsigned long)oldUiCount,
		(unsigned long)ReadU32(self, 0x348),
		(unsigned long)oldJoinState,
		(unsigned long)ReadU32(self, 0x34c));
	return true;
}

const char* LanSelectEventName(std::uint32_t hash)
{
	switch (hash) {
	case 0xf1943ca3: return "refresh";
	case 0xe1fde1d1: return "back";
	case 0x7eabca56: return "list-select-or-mouse";
	case 0x6b7baa6f: return "join-or-accept";
	case 0x406415e3: return "lan-row-host-select";
	case 0x0c407210: return "lan-row-select-alt";
	case 0x35f8620b: return "lan-scroll-text";
	case 0x7345710f: return "lan-clear-browser-state";
	case 0x72619778: return "page-up";
	case 0x911c0a4b: return "page-down";
	case 0x9120409e: return "row-up";
	case 0xb5971bf1: return "row-down";
	case 0x9803f6e2: return "scroll";
	case 0xc519bfc0: return "lan-open-selected";
	case 0xc519bfc3: return "mode-toggle";
	case 0xc519bfc4: return "position-highlight";
	case 0xc98356ba: return "lan-browser-poll";
	case 0xd9feec59: return "tab-next";
	case 0x5073ef13: return "tab-prev";
	case 0x911ab364: return "ui-confirm";
	case 0x00ca4ce1: return "animation-complete";
	default: return "unknown";
	}
}

void LogLanSelectEvent(const char* tag, void* self, std::uint32_t hash, std::uint32_t arg2, std::uint32_t arg3, std::uint32_t arg4)
{
	LogLine(
		"%s lan-event this=%p hash=0x%08lX name=%s arg2=0x%08lX arg3=0x%08lX arg4=0x%08lX page=%lu top=%lu selected=%lu ui_count=%lu join_state=%lu list_count=%d",
		tag ? tag : "lan-select",
		self,
		(unsigned long)hash,
		LanSelectEventName(hash),
		(unsigned long)arg2,
		(unsigned long)arg3,
		(unsigned long)arg4,
		self ? (unsigned long)ReadU32(self, 0x30) : 0,
		self ? (unsigned long)ReadU32(self, 0x33c) : 0,
		self ? (unsigned long)ReadU32(self, 0x340) : 0,
		self ? (unsigned long)ReadU32(self, 0x348) : 0,
		self ? (unsigned long)ReadU32(self, 0x34c) : 0,
		self ? CountLanServerList(self) : 0);
}

void LogLanSelectJoinState(const char* tag, void* self)
{
	LogLine(
		"%s lan-join-state this=%p page=0x%08lX mode34=0x%08lX async38=0x%08lX async44=0x%08lX row40=0x%08lX top33c=%lu selected340=%lu count348=%lu join_state34c=%lu pos350=%lu focus354=%lu list_count=%d",
		tag ? tag : "lan-select",
		self,
		self ? (unsigned long)ReadU32(self, 0x30) : 0,
		self ? (unsigned long)ReadU32(self, 0x34) : 0,
		self ? (unsigned long)ReadU32(self, 0x38) : 0,
		self ? (unsigned long)ReadU32(self, 0x44) : 0,
		self ? (unsigned long)ReadU32(self, 0x40) : 0,
		self ? (unsigned long)ReadU32(self, 0x33c) : 0,
		self ? (unsigned long)ReadU32(self, 0x340) : 0,
		self ? (unsigned long)ReadU32(self, 0x348) : 0,
		self ? (unsigned long)ReadU32(self, 0x34c) : 0,
		self ? (unsigned long)ReadU32(self, 0x350) : 0,
		self ? (unsigned long)ReadU32(self, 0x354) : 0,
		self ? CountLanServerList(self) : 0);
}
