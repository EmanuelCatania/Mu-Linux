#pragma once
#include "net/Protocol/GameServerProtocol.h"
#include <string>

// Consulta de porcentaje/costo del DLL, sobre la ventana Chaos existente.
class CChaosMixRates
{
public:
    // Lo que manda el server de una receta agregada (Recipes.toml).
    struct Recipe {
        int Width = 0;
        int Height = 0;
        std::string Name;
        std::string Help[Proto::CHAOS_RECIPE_HELP_LINES];
        int Lines = 0;
    };
    void Reset();
    void Update(int type, const ITEM* items);
    // hasType: el server mandó la receta que reconoció (0.97.50 en adelante).
    // recipe: los datos de una receta agregada, si la respuesta los trae.
    void Receive(const Proto::PMSG_CHAOS_MIX_RATE_SEND& packet, bool hasType, int type, const Recipe* recipe = nullptr);
    // La receta agregada que el server reconoció en la grilla, o nullptr.
    const Recipe* GetRecipe() const;
    bool Get(int& rate, int& money) const;
    // La receta de la grilla: la del server si ya respondió por este contenido,
    // si no la que reconoció el cliente.
    int ResolveType(int localType) const;
    static bool Supports(int type) {
        return (type >= 1 && type <= 8) || type == 11 ||
               (type >= Proto::CHAOS_RECIPE_FIRST_ID && type <= Proto::CHAOS_RECIPE_LAST_ID);
    }

private:
    struct Material {
        short Type = -1;
        int Level = 0;
        BYTE Durability = 0;
        BYTE Option = 0;
    };
    Material m_Items[32]{};
    int m_Type = 0;
    unsigned int m_Revision = 0;
    unsigned int m_PendingRevision = 0;
    enum class State { Idle, Waiting, Ready };
    State m_State = State::Idle;
    DWORD m_LastSent = 0;
    unsigned int m_Attempts = 0;
    int m_Rate = 0;
    int m_Money = 0;
    bool m_HasServerType = false;
    int m_ServerType = 0;
    bool m_HasRecipe = false;
    Recipe m_Recipe;
};

extern CChaosMixRates gChaosMixRates;
