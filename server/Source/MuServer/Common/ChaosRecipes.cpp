#include "ChaosRecipes.h"
#include "Utf8Path.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>

// Igual que EventInvasions.cpp: sin excepciones, los errores vuelven en parse_result.
#define TOML_EXCEPTIONS 0
#include "tomlplusplus/toml.hpp"

namespace fs = std::filesystem;

namespace
{
	const char* const kLanguages[CHAOS_RECIPE_LANGUAGES] = { "Eng", "Spn", "Por" };

	std::string Where(const std::string& file, const toml::node& node)
	{
		return file + ":" + std::to_string(node.source().begin.line) + ": ";
	}

	bool CheckKeys(const toml::table& table, std::initializer_list<const char*> allowed, const std::string& where, std::string& error)
	{
		for (const auto& [key, node] : table)
		{
			bool ok = false;

			for (const char* name : allowed)
			{
				ok = ok || key.str() == name;
			}

			if (!ok)
			{
				error = where + "campo desconocido '" + std::string(key.str()) + "'";

				return false;
			}
		}

		return true;
	}

	// Entero opcional; sin la clave queda `out` como estaba.
	bool ReadInt(const toml::table& table, const char* key, int minValue, int maxValue, int& out, const std::string& where, std::string& error)
	{
		const toml::node* node = table.get(key);

		if (node == nullptr)
		{
			return true;
		}

		std::optional<int64_t> value = node->value<int64_t>();

		if (!value || *value < minValue || *value > maxValue)
		{
			error = where + "'" + key + "' tiene que ser un entero entre " + std::to_string(minValue) + " y " + std::to_string(maxValue);

			return false;
		}

		out = (int)*value;

		return true;
	}

	// [a, b] con los dos enteros entre minValue y maxValue.
	bool ReadPair(const toml::table& table, const char* key, bool required, int minValue, int maxValue, int& a, int& b, const std::string& where, std::string& error)
	{
		const toml::node* node = table.get(key);

		if (node == nullptr)
		{
			if (required)
			{
				error = where + "falta '" + key + "'";

				return false;
			}

			return true;
		}

		const toml::array* array = node->as_array();

		std::optional<int64_t> first = (array != nullptr && array->size() == 2) ? (*array)[0].value<int64_t>() : std::nullopt;

		std::optional<int64_t> second = (array != nullptr && array->size() == 2) ? (*array)[1].value<int64_t>() : std::nullopt;

		if (!first || !second || *first < minValue || *first > maxValue || *second < minValue || *second > maxValue)
		{
			error = where + "'" + key + "' tiene que ser [a, b] con enteros entre " + std::to_string(minValue) + " y " + std::to_string(maxValue);

			return false;
		}

		a = (int)*first;

		b = (int)*second;

		return true;
	}

	bool CheckText(const std::string& text, const std::string& where, const char* what, std::string& error)
	{
		if (text.empty() || text.size() >= CHAOS_RECIPE_TEXT_SIZE)
		{
			error = where + what + " tiene que tener entre 1 y " + std::to_string(CHAOS_RECIPE_TEXT_SIZE - 1) + " bytes (mide " + std::to_string(text.size()) + ")";

			return false;
		}

		for (unsigned char c : text)
		{
			if (c < 0x20)
			{
				error = where + what + " no puede llevar saltos de línea ni caracteres de control";

				return false;
			}
		}

		return true;
	}

	// "texto" (todos los idiomas) o { Eng = "...", Spn = "...", Por = "..." }.
	bool ReadName(const toml::table& table, std::string (&out)[CHAOS_RECIPE_LANGUAGES], const std::string& where, std::string& error)
	{
		const toml::node* node = table.get("name");

		if (node == nullptr)
		{
			error = where + "falta 'name'";

			return false;
		}

		if (std::optional<std::string> text = node->value<std::string>())
		{
			for (std::string& name : out)
			{
				name = *text;
			}
		}
		else if (const toml::table* byLang = node->as_table())
		{
			if (!CheckKeys(*byLang, { "Eng", "Spn", "Por" }, where, error))
			{
				return false;
			}

			for (int lang = 0; lang < CHAOS_RECIPE_LANGUAGES; lang++)
			{
				const toml::node* text = byLang->get(kLanguages[lang]);

				if (text != nullptr)
				{
					std::optional<std::string> value = text->value<std::string>();

					if (!value)
					{
						error = where + "'name." + kLanguages[lang] + "' tiene que ser un texto";

						return false;
					}

					out[lang] = *value;
				}
			}

			if (out[0].empty())
			{
				error = where + "'name' tiene que traer al menos el inglés (Eng)";

				return false;
			}
		}
		else
		{
			error = where + "'name' es un texto o { Eng = \"...\", Spn = \"...\", Por = \"...\" }";

			return false;
		}

		for (int lang = 0; lang < CHAOS_RECIPE_LANGUAGES; lang++)
		{
			if (out[lang].empty())
			{
				out[lang] = out[0];
			}

			if (!CheckText(out[lang], where, "el nombre", error))
			{
				return false;
			}
		}

		return true;
	}

	bool ReadLines(const toml::node& node, std::vector<std::string>& out, const std::string& where, const char* key, std::string& error)
	{
		const toml::array* array = node.as_array();

		if (array == nullptr || array->size() > CHAOS_RECIPE_HELP_LINES)
		{
			error = where + "'" + key + "' es una lista de hasta " + std::to_string(CHAOS_RECIPE_HELP_LINES) + " líneas";

			return false;
		}

		for (const toml::node& line : *array)
		{
			std::optional<std::string> text = line.value<std::string>();

			if (!text || !CheckText(*text, where, "cada línea de ayuda", error))
			{
				if (!text)
				{
					error = where + "cada línea de '" + key + "' es un texto";
				}

				return false;
			}

			out.push_back(*text);
		}

		return true;
	}

	// ["...", "..."] (todos los idiomas) o { Eng = [...], Spn = [...], Por = [...] }.
	bool ReadHelp(const toml::table& table, std::vector<std::string> (&out)[CHAOS_RECIPE_LANGUAGES], const std::string& where, std::string& error)
	{
		const toml::node* node = table.get("help");

		if (node == nullptr)
		{
			return true;
		}

		if (const toml::table* byLang = node->as_table())
		{
			if (!CheckKeys(*byLang, { "Eng", "Spn", "Por" }, where, error))
			{
				return false;
			}

			for (int lang = 0; lang < CHAOS_RECIPE_LANGUAGES; lang++)
			{
				const toml::node* lines = byLang->get(kLanguages[lang]);

				if (lines != nullptr && !ReadLines(*lines, out[lang], where, "help", error))
				{
					return false;
				}
			}

			for (int lang = 1; lang < CHAOS_RECIPE_LANGUAGES; lang++)
			{
				if (byLang->get(kLanguages[lang]) == nullptr)
				{
					out[lang] = out[0];
				}
			}

			return true;
		}

		if (!ReadLines(*node, out[0], where, "help", error))
		{
			return false;
		}

		out[1] = out[0];

		out[2] = out[0];

		return true;
	}

	bool ReadIngredient(const toml::node& node, ChaosRecipeIngredient& ing, const std::string& file, std::string& error)
	{
		const std::string where = Where(file, node);

		const toml::table* table = node.as_table();

		if (table == nullptr)
		{
			error = where + "cada ingrediente es { item = [sección, índice], count = N, level = [mín, máx] }";

			return false;
		}

		if (!CheckKeys(*table, { "item", "count", "level" }, where, error))
		{
			return false;
		}

		return ReadPair(*table, "item", true, 0, 511, ing.Section, ing.Index, where, error)
			&& ReadInt(*table, "count", 1, 32, ing.Count, where, error)
			&& ReadPair(*table, "level", false, 0, 15, ing.MinLevel, ing.MaxLevel, where, error);
	}

	bool ReadRecipe(const toml::table& table, ChaosRecipeDef& def, const std::string& file, std::string& error)
	{
		const std::string where = Where(file, table);

		def.Source = file + ":" + std::to_string(table.source().begin.line);

		if (!CheckKeys(table, { "id", "name", "help", "rate", "money", "ingredients", "result" }, where, error))
		{
			return false;
		}

		def.Id = -1;

		if (!ReadInt(table, "id", CHAOS_RECIPE_FIRST_ID, CHAOS_RECIPE_LAST_ID, def.Id, where, error))
		{
			return false;
		}

		if (def.Id < 0)
		{
			error = where + "falta 'id' (" + std::to_string(CHAOS_RECIPE_FIRST_ID) + " a " + std::to_string(CHAOS_RECIPE_LAST_ID) + ")";

			return false;
		}

		def.Rate = -1;

		def.Money = 0;

		if (!ReadName(table, def.Name, where, error) || !ReadHelp(table, def.Help, where, error)
			|| !ReadInt(table, "rate", 0, 100, def.Rate, where, error) || !ReadInt(table, "money", 0, 2000000000, def.Money, where, error))
		{
			return false;
		}

		if (def.Rate < 0)
		{
			error = where + "falta 'rate' (0 a 100)";

			return false;
		}

		const toml::node* list = table.get("ingredients");

		const toml::array* ingredients = (list != nullptr) ? list->as_array() : nullptr;

		if (ingredients == nullptr || ingredients->empty() || ingredients->size() > CHAOS_RECIPE_MAX_INGREDIENTS)
		{
			error = where + "'ingredients' es una lista de 1 a " + std::to_string(CHAOS_RECIPE_MAX_INGREDIENTS) + " ingredientes";

			return false;
		}

		int total = 0;

		for (const toml::node& node : *ingredients)
		{
			ChaosRecipeIngredient ing;

			if (!ReadIngredient(node, ing, file, error))
			{
				return false;
			}

			if (ing.MinLevel > ing.MaxLevel)
			{
				error = Where(file, node) + "'level' va [mínimo, máximo]";

				return false;
			}

			total += ing.Count;

			def.Ingredients.push_back(ing);
		}

		if (total > 32)
		{
			error = where + "los ingredientes suman " + std::to_string(total) + " items y el chaos box tiene 32 lugares";

			return false;
		}

		const toml::node* resultNode = table.get("result");

		const toml::table* result = (resultNode != nullptr) ? resultNode->as_table() : nullptr;

		if (result == nullptr)
		{
			error = where + "falta 'result' = { item = [sección, índice], level = N }";

			return false;
		}

		const std::string resultWhere = Where(file, *result);

		return CheckKeys(*result, { "item", "level" }, resultWhere, error)
			&& ReadPair(*result, "item", true, 0, 511, def.ResultSection, def.ResultIndex, resultWhere, error)
			&& ReadInt(*result, "level", 0, 15, def.ResultLevel, resultWhere, error);
	}
}

std::string ChaosRecipes_Path(const std::string& dataDir)
{
	return PathToUtf8(Utf8Path(dataDir) / "ChaosMix" / "Recipes.toml");
}

bool ChaosRecipes_Load(const std::string& dataDir, std::vector<ChaosRecipeDef>& out, std::string& error)
{
	out.clear();

	const std::string path = ChaosRecipes_Path(dataDir);

	std::error_code ec;

	if (!fs::is_regular_file(Utf8Path(path), ec))
	{
		return true;
	}

	toml::parse_result parsed = toml::parse_file(path);

	if (!parsed)
	{
		const toml::parse_error& e = parsed.error();

		error = path + ":" + std::to_string(e.source().begin.line) + ": " + std::string(e.description());

		return false;
	}

	const toml::table& root = parsed.table();

	for (const auto& [key, node] : root)
	{
		if (key.str() != "recipe")
		{
			error = Where(path, node) + "sección desconocida '" + std::string(key.str()) + "' (vale [[recipe]])";

			return false;
		}
	}

	const toml::node* list = root.get("recipe");

	if (list == nullptr)
	{
		return true;
	}

	const toml::array* array = list->as_array();

	if (array == nullptr)
	{
		error = Where(path, *list) + "'recipe' va como [[recipe]]";

		return false;
	}

	std::map<int, std::string> used;

	for (const toml::node& item : *array)
	{
		const toml::table* table = item.as_table();

		if (table == nullptr)
		{
			error = Where(path, item) + "cada receta es una tabla [[recipe]]";

			return false;
		}

		ChaosRecipeDef def;

		if (!ReadRecipe(*table, def, path, error))
		{
			return false;
		}

		if (used.count(def.Id) != 0)
		{
			error = def.Source + ": la receta " + std::to_string(def.Id) + " ya está en " + used[def.Id];

			return false;
		}

		used[def.Id] = def.Source;

		out.push_back(def);
	}

	return true;
}
