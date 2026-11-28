#pragma once

// Recetas del Chaos agregadas, en TOML (Data/ChaosMix/Recipes.toml).
//
// Las nueve recetas del 0.97k (CHAOS_MIX_* del GameServer) siguen en el código,
// con sus porcentajes en GameServerInfo - ChaosMix.dat.  Este archivo agrega
// recetas nuevas: qué items lleva el chaos box, qué porcentaje y qué costo
// tiene, y qué item sale.  El server las reconoce y le manda al cliente el
// nombre y la ayuda del panel en el idioma de cada jugador.
//
// Este módulo no depende del GameServer: lo usan CChaosBox y mu-data.

#include <string>
#include <vector>

#define CHAOS_RECIPE_FIRST_ID 20       // 1..11 son las del 0.97k
#define CHAOS_RECIPE_LAST_ID 127       // el cliente guarda el número en un byte con signo
#define CHAOS_RECIPE_LANGUAGES 3       // Eng, Spn, Por (eLanguages del GameServer)
#define CHAOS_RECIPE_TEXT_SIZE 48      // nombre y cada línea de ayuda: 47 bytes y el 0
#define CHAOS_RECIPE_HELP_LINES 3
#define CHAOS_RECIPE_MAX_INGREDIENTS 8

struct ChaosRecipeIngredient
{
	int Section = 0;                   // [sección, índice] de Item.txt
	int Index = 0;
	int Count = 1;                     // cuántos tiene que haber exactamente
	int MinLevel = 0;
	int MaxLevel = 15;
};

struct ChaosRecipeDef
{
	int Id = 0;

	// Por idioma (0 Eng, 1 Spn, 2 Por).  Un idioma sin texto usa el inglés.
	std::string Name[CHAOS_RECIPE_LANGUAGES];

	std::vector<std::string> Help[CHAOS_RECIPE_LANGUAGES];

	int Rate = 0;                      // porcentaje de éxito, 0 a 100

	int Money = 0;                     // zen que cuesta mezclar

	std::vector<ChaosRecipeIngredient> Ingredients;

	int ResultSection = 0;             // el item que sale
	int ResultIndex = 0;

	int ResultLevel = 0;               // sale un solo item: el server vacía el chaos box al crearlo

	std::string Source;                // archivo:línea, para los mensajes de error
};

std::string ChaosRecipes_Path(const std::string& dataDir);

// Lee Recipes.toml.  Sin el archivo no hay recetas agregadas (y no es error).
// Falla si un número se repite o un texto no entra en el panel del cliente.
bool ChaosRecipes_Load(const std::string& dataDir, std::vector<ChaosRecipeDef>& out, std::string& error);
