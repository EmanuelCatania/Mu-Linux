#pragma once

#include "ChaosRecipes.h"

#include <vector>

#include "ItemManager.h"
#include "ProtocolDefines.h"
#include "User.h"

enum eChaosMixNumber
{
	CHAOS_MIX_CHAOS_ITEM = 1,
	CHAOS_MIX_DEVIL_SQUARE = 2,
	CHAOS_MIX_PLUS_ITEM_LEVEL1 = 3,
	CHAOS_MIX_PLUS_ITEM_LEVEL2 = 4,
	CHAOS_MIX_DINORANT = 5,
	CHAOS_MIX_FRUIT = 6,
	CHAOS_MIX_WING2 = 7,
	CHAOS_MIX_BLOOD_CASTLE = 8,
	CHAOS_MIX_WING1 = 11,
};

//**********************************************//
//************ GameServer -> Client ************//
//**********************************************//

struct PMSG_CHAOS_MIX_SEND
{
	PBMSG_HEAD header; // C1:86
	BYTE result;
	BYTE ItemInfo[MAX_ITEM_INFO];
};

struct PMSG_CHAOS_MIX_RATE_SEND
{
	PBMSG_HEAD header; // C1:88
	DWORD rate;
	DWORD money;
	// Desde la 0.97.50: la receta que reconoció el server (GetChaosMixType).
	// Un cliente anterior lee sólo los 12 primeros bytes.
	int type;
};

// La misma respuesta para una receta agregada (Data/ChaosMix/Recipes.toml):
// agrega lo que el cliente no tiene en su Text.bmd, en el idioma del jugador.
struct PMSG_CHAOS_MIX_RECIPE_SEND
{
	PBMSG_HEAD header; // C1:88
	DWORD rate;
	DWORD money;
	int type;
	BYTE width;		// tamaño del item que sale, para ver si entra en el inventario
	BYTE height;
	BYTE lines;		// líneas de ayuda
	BYTE reserved;
	char name[CHAOS_RECIPE_TEXT_SIZE];
	char help[CHAOS_RECIPE_HELP_LINES][CHAOS_RECIPE_TEXT_SIZE];
};

//**********************************************//
//************ Client -> GameServer ************//
//**********************************************//

struct PMSG_CHAOS_MIX_RECV
{
	PBMSG_HEAD header; // C1:86
	BYTE type;
};

struct PMSG_CHAOS_MIX_RATE_RECV
{
	PBMSG_HEAD header; // C1:88
	int type;
};

//**********************************************//
//**********************************************//
//**********************************************//

class CChaosBox
{
public:

	CChaosBox();

	~CChaosBox();

	void ChaosBoxInit(LPOBJ lpObj);

	void ChaosBoxItemDown(LPOBJ lpObj, int slot);

	void ChaosBoxItemKeep(LPOBJ lpObj, int slot);

	void GCChaosBoxSend(LPOBJ lpObj);

	void CGChaosMixCloseRecv(int aIndex);

	void CGChaosMixRecv(PMSG_CHAOS_MIX_RECV* lpMsg, int aIndex);

	void GCChaosMixSend(int aIndex, BYTE result, CItem* lpItem);

	void CGChaosMixRateRecv(PMSG_CHAOS_MIX_RATE_RECV* lpMsg, int aIndex);

	bool GetChaosMixRateAndMoney(int aIndex, int type, DWORD& rate, DWORD& money);

	int GetChaosMixType(LPOBJ lpObj);

	// Recetas agregadas (Data/ChaosMix/Recipes.toml).
	void LoadRecipes(const char* dataDir);

	const ChaosRecipeDef* FindRecipe(int id) const;

	const ChaosRecipeDef* MatchRecipe(LPOBJ lpObj) const;

	void RecipeMix(LPOBJ lpObj, const ChaosRecipeDef& recipe);

	void ChaosItemMix(LPOBJ lpObj);
	bool GetChaosItemMixRates(int aIndex, DWORD& rate, DWORD& money);

	void DevilSquareMix(LPOBJ lpObj);
	bool GetDevilSquareMixRates(int aIndex, DWORD& rate, DWORD& money, int& EyeLevel);

	void PlusItemLevelMix(LPOBJ lpObj, int type);
	bool GetPlusItemLevelMixRates(int aIndex, DWORD& rate, DWORD& money, int type, int& ItemSlot);

	void DinorantMix(LPOBJ lpObj);
	bool GetDinorantMixRates(int aIndex, DWORD& rate, DWORD& money);

	void FruitMix(LPOBJ lpObj);
	bool GetFruitMixRates(int aIndex, DWORD& rate, DWORD& money);

	void Wing1Mix(LPOBJ lpObj);
	bool GetWing1MixRates(int aIndex, DWORD& rate, DWORD& money);

	void BloodCastleMix(LPOBJ lpObj);
	bool GetBloodCastleMixRates(int aIndex, DWORD& rate, DWORD& money, int& ScrollLevel);

	void Wing2Mix(LPOBJ lpObj);
	bool GetWing2MixRates(int aIndex, DWORD& rate, DWORD& money);

private:

	std::vector<ChaosRecipeDef> m_Recipes;
};

extern CChaosBox gChaosBox;