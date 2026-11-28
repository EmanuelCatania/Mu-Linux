#pragma once
#include <cstdint>

struct ITEM;

// Valores de un item calculados por el server (C1:F3:EB): requisitos,
// durabilidad máxima, precios y reparación.  El cliente los pide por lotes
// para los items que tiene a la vista y los usa en lugar de sus fórmulas; un
// item del que todavía no hay respuesta, o un server que no la manda, sigue con
// el cálculo local.
struct ItemServerValues
{
    std::uint16_t RequireStrength = 0;
    std::uint16_t RequireDexterity = 0;
    std::uint16_t RequireVitality = 0;
    std::uint16_t RequireEnergy = 0;
    std::uint16_t RequireLevel = 0;
    std::uint16_t MaxDurability = 0;
    std::uint32_t BuyMoney = 0;
    std::uint32_t SellMoney = 0;
    std::uint32_t RepairMoney = 0;       // en el NPC
    std::uint32_t RepairMoneySelf = 0;   // reparación propia
};

namespace ItemServerInfo
{
    // Lo que respondió el server para ese item (tipo, nivel y opciones,
    // durabilidad), o nullptr; en ese caso lo pide.
    const ItemServerValues* Find(const ITEM* item);

    // Pisa los requisitos del item con los del server si ya están.
    void Apply(ITEM* item);

    // Manda los pedidos pendientes (una vez por frame).
    void Tick();

    void Receive(const std::uint8_t* msg, int size);

    // Al cambiar de personaje o de server.
    void Reset();
}
