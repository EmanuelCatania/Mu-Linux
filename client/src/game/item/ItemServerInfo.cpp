#include "stdafx.h"
#include "game/item/ItemServerInfo.h"
#include "game/item/ItemTypes.h"
#include "game/item/ItemDefines.h"
#include "net/Network.h"
#include "platform/ITimer.h"
#include "globals.h"

#include <cstring>
#include <map>
#include <vector>

extern "C" BYTE Inventory[];
extern "C" BYTE OffsetInventoryItems[];
extern "C" BYTE OffsetTradeItems[];
extern "C" BYTE OffsetMixItems[];
extern "C" BYTE ShopItems[];
extern "C" BYTE OffsetWarehouseItems[];

namespace
{
// ItemManager.h del server: PMSG_ITEM_INFO_QUERY_RECV / PMSG_ITEM_INFO_ANSWER_SEND.
constexpr int kMaxPerPacket = 6;
constexpr DWORD kSendInterval = 150;    // ms entre pedidos
constexpr DWORD kAnswerTimeout = 5000;  // un pedido sin respuesta se vuelve a hacer
constexpr size_t kMaxPending = 512;

#pragma pack(push, 1)
struct Query
{
    std::uint16_t Index;
    std::uint8_t Level;
    std::uint8_t Option1;
    std::uint8_t Durability;
    std::uint8_t Reserved[3];
};
struct Answer
{
    Query Item;
    std::uint16_t RequireStrength, RequireDexterity, RequireVitality, RequireEnergy, RequireLevel, MaxDurability;
    std::uint32_t BuyMoney, SellMoney, RepairMoney, RepairMoneySelf;
};
#pragma pack(pop)
static_assert(sizeof(Query) == 8, "ITEM_INFO_QUERY");
static_assert(sizeof(Answer) == 36, "ITEM_INFO_ANSWER");
constexpr int kHeader = 8;   // C1 size F3 EB, count, 3 de relleno

// Respuestas; Valid = false si el server no conoce el item (no se vuelve a pedir).
struct Entry { ItemServerValues Values; bool Valid = false; };
std::map<std::uint64_t, Entry> s_Values;
std::vector<std::uint64_t> s_Pending;            // pedidos por mandar
std::map<std::uint64_t, DWORD> s_InFlight;      // mandados y sin respuesta
DWORD s_LastSent = 0;
bool s_Answered = false;      // el server respondió alguna vez
int s_Unanswered = 0;         // pedidos vencidos sin que el server responda nunca
bool s_Disabled = false;      // server anterior: no pregunta más hasta el próximo Reset

std::uint64_t KeyOf(int type, int level, int option1, int durability)
{
    return (std::uint64_t)(std::uint16_t)type | ((std::uint64_t)(std::uint8_t)level << 16) |
           ((std::uint64_t)(std::uint8_t)option1 << 24) | ((std::uint64_t)(std::uint8_t)durability << 32);
}

std::uint64_t KeyOf(const ITEM* item)
{
    return KeyOf(item->Type, item->Level, item->Option1, item->Durability);
}

void ApplyValues(ITEM* item, const ItemServerValues& v)
{
    item->RequireStrength = v.RequireStrength;
    item->RequireDexterity = v.RequireDexterity;
    item->RequireEnergy = v.RequireEnergy;
    item->RequireLevel = v.RequireLevel;
}

// Los items que ya están cargados toman los requisitos apenas llega la respuesta.
void ApplyToPools(std::uint64_t key, const ItemServerValues& v)
{
    struct Pool { BYTE* base; int slots; };
    const Pool pools[] = {
        { OffsetInventoryItems, 64 }, { Inventory, 64 }, { OffsetTradeItems, 64 },
        { OffsetMixItems, 64 }, { ShopItems, 120 }, { OffsetWarehouseItems, 120 },
        { CharacterMachine ? static_cast<BYTE*>(CharacterMachine) + 536 : nullptr, 12 },
    };
    for (const Pool& pool : pools) {
        if (!pool.base) continue;
        for (int i = 0; i < pool.slots; ++i) {
            ITEM* item = reinterpret_cast<ITEM*>(pool.base + i * sizeof(ITEM));
            if (item->Type >= 0 && KeyOf(item) == key) ApplyValues(item, v);
        }
    }
}
} // namespace

const ItemServerValues* ItemServerInfo::Find(const ITEM* item)
{
    if (!item || item->Type < 0 || item->Type >= ITEM_MAX_EX) return nullptr;
    const std::uint64_t key = KeyOf(item);
    const auto it = s_Values.find(key);
    if (it != s_Values.end()) return it->second.Valid ? &it->second.Values : nullptr;
    if (s_Disabled || s_InFlight.count(key) || s_Pending.size() >= kMaxPending) return nullptr;
    for (std::uint64_t pending : s_Pending)
        if (pending == key) return nullptr;
    s_Pending.push_back(key);
    return nullptr;
}

void ItemServerInfo::Apply(ITEM* item)
{
    if (const ItemServerValues* v = Find(item)) ApplyValues(item, *v);
}

void ItemServerInfo::Tick()
{
    if (s_Disabled || SceneFlag != 5) return;
    const DWORD now = Platform::GetTimer().Ticks();
    // Lo vencido se vuelve a pedir.  Si el server nunca respondió, es uno
    // anterior a la 0.97.50: se deja de preguntar.
    for (auto it = s_InFlight.begin(); it != s_InFlight.end();) {
        if (now - it->second < kAnswerTimeout) { ++it; continue; }
        if (!s_Answered && ++s_Unanswered >= 3) { s_Disabled = true; s_InFlight.clear(); s_Pending.clear(); return; }
        if (s_Pending.size() < kMaxPending) s_Pending.push_back(it->first);
        it = s_InFlight.erase(it);
    }
    if (s_Pending.empty() || now - s_LastSent < kSendInterval) return;

    BYTE packet[kHeader + kMaxPerPacket * sizeof(Query)] = {};
    int count = 0;
    while (count < kMaxPerPacket && !s_Pending.empty()) {
        const std::uint64_t key = s_Pending.front();
        s_Pending.erase(s_Pending.begin());
        Query query = {};
        query.Index = (std::uint16_t)(key & 0xFFFF);
        query.Level = (std::uint8_t)(key >> 16);
        query.Option1 = (std::uint8_t)(key >> 24);
        query.Durability = (std::uint8_t)(key >> 32);
        memcpy(packet + kHeader + count * sizeof(Query), &query, sizeof(query));
        s_InFlight[key] = now;
        ++count;
    }
    const int length = kHeader + count * (int)sizeof(Query);
    packet[0] = 0xC1;
    packet[1] = (BYTE)length;
    packet[2] = 0xF3;
    packet[3] = 0xEB;
    packet[4] = (BYTE)count;
    gNetwork.SendC1(packet, length);
    s_LastSent = now;
}

void ItemServerInfo::Receive(const std::uint8_t* msg, int size)
{
    if (size < kHeader) return;
    const int count = msg[4];
    if (count > kMaxPerPacket || size < kHeader + count * (int)sizeof(Answer)) return;
    s_Answered = true;
    for (int i = 0; i < count; ++i) {
        Answer answer;
        memcpy(&answer, msg + kHeader + i * sizeof(Answer), sizeof(answer));
        const std::uint64_t key = KeyOf((std::int16_t)answer.Item.Index, answer.Item.Level, answer.Item.Option1, answer.Item.Durability);
        s_InFlight.erase(key);
        if (answer.Item.Reserved[0] != 1) { s_Values[key] = Entry{}; continue; }
        ItemServerValues v;
        v.RequireStrength = answer.RequireStrength;
        v.RequireDexterity = answer.RequireDexterity;
        v.RequireVitality = answer.RequireVitality;
        v.RequireEnergy = answer.RequireEnergy;
        v.RequireLevel = answer.RequireLevel;
        v.MaxDurability = answer.MaxDurability;
        v.BuyMoney = answer.BuyMoney;
        v.SellMoney = answer.SellMoney;
        v.RepairMoney = answer.RepairMoney;
        v.RepairMoneySelf = answer.RepairMoneySelf;
        s_Values[key] = Entry{ v, true };
        ApplyToPools(key, v);
    }
}

void ItemServerInfo::Reset()
{
    s_Values.clear();
    s_Pending.clear();
    s_InFlight.clear();
    s_LastSent = 0;
    s_Answered = false;
    s_Unanswered = 0;
    s_Disabled = false;
}
