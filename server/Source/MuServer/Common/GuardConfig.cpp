// Ver GuardConfig.h.  Sin stdafx.h: lo compilan el ConnectServer, el GameServer
// y mu-guard.

#include "GuardConfig.h"

#define TOML_EXCEPTIONS 0
#include "tomlplusplus/toml.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <cctype>
#include <filesystem>
#include <mutex>

namespace
{
	std::mutex s_Lock;
	std::string s_Path;
	std::filesystem::file_time_type s_Time;
	bool s_Exists = false;
	GuardSettings s_Settings;

	constexpr std::size_t GuardRuleMax = 255;   // GuardProto::MaxRule

	// Lo que verifica FILES: los .bmd, .toml y .att de Content/ (mu-guard files
	// usa el mismo criterio).
	bool FilesTracked(const std::string& path)
	{
		if (path.compare(0, 8, "Content/") != 0)
		{
			return false;
		}

		const std::size_t dot = path.find_last_of('.');

		if (dot == std::string::npos)
		{
			return false;
		}

		std::string extension = path.substr(dot);

		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return (char)std::tolower(c); });

		return extension == ".bmd" || extension == ".toml" || extension == ".att";
	}

	// Un string JSON desde la comilla de apertura (at): devuelve el texto y deja
	// at después de la comilla de cierre.  Sólo los escapes que usa mu-patch.
	bool JsonString(const std::string& text, std::size_t& at, std::string& out)
	{
		if (at >= text.size() || text[at] != '"')
		{
			return false;
		}

		out.clear();

		for (at++; at < text.size(); at++)
		{
			const char ch = text[at];

			if (ch == '"')
			{
				at++;

				return true;
			}

			if (ch == '\\' && at + 1 < text.size())
			{
				const char next = text[++at];

				out += next == 'n' ? '\n' : (next == 't' ? '\t' : next);

				continue;
			}

			out += ch;
		}

		return false;
	}

	// El valor string de "key" a partir de at (busca hasta `limit`).
	bool JsonField(const std::string& text, const char* key, std::size_t from, std::size_t limit, std::string& out)
	{
		const std::string needle = std::string("\"") + key + "\"";

		std::size_t at = text.find(needle, from);

		if (at == std::string::npos || at >= limit)
		{
			return false;
		}

		at = text.find('"', text.find(':', at + needle.size()));

		return at < limit && JsonString(text, at, out);
	}

	// Overlay publicado (Data/Patches/published/manifest.json, lo escribe
	// mu-patch): los archivos que agrega o cambia y los que borra.  El
	// manifiesto es local y lo firmó este mismo server.
	void ReadOverlay(const std::string& path, std::map<std::string, std::string>& files)
	{
		std::ifstream in(path, std::ios::binary);

		const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

		const std::size_t list = text.find("\"files\"");

		if (list != std::string::npos)
		{
			std::size_t at = list;

			for (;;)
			{
				const std::size_t entry = text.find("\"path\"", at + 1);

				if (entry == std::string::npos)
				{
					break;
				}

				const std::size_t next = text.find("\"path\"", entry + 6);

				const std::size_t limit = next == std::string::npos ? text.size() : next;

				std::string file, hash;

				if (JsonField(text, "path", entry, limit, file) && JsonField(text, "hash", entry, limit, hash) &&
					hash.compare(0, 12, "blake2b-256:") == 0 && hash.size() == 12 + 64 && FilesTracked(file))
				{
					files[file] = hash.substr(12);
				}

				at = entry;
			}
		}

		const std::size_t deleted = text.find("\"deleted\"");

		if (deleted != std::string::npos)
		{
			std::size_t at = text.find('[', deleted);

			const std::size_t end = text.find(']', at);

			while (at != std::string::npos && at < end)
			{
				at = text.find('"', at);

				std::string file;

				if (at == std::string::npos || at >= end || !JsonString(text, at, file))
				{
					break;
				}

				files.erase(file);
			}
		}
	}

	const char* const kDetectionNames[] = { "?", "DEBUGGER", "INJECT", "MEMEDIT", "MEMSCAN", "PATCH", "FILES", "TOOL", "KILLED" };

	constexpr int kDetections = (int)(sizeof(kDetectionNames) / sizeof(kDetectionNames[0]));

	std::uint32_t DefaultWarningCodes()
	{
		// DEBUGGER, INJECT, MEMEDIT, PATCH y KILLED.  MEMSCAN no: un antivirus que
		// lee la memoria del juego lo dispara igual que un editor.
		return (1u << 1) | (1u << 2) | (1u << 3) | (1u << 5) | (1u << 8);
	}

	int ClampInt(std::int64_t value, int low, int high)
	{
		return (int)(value < low ? low : (value > high ? high : value));
	}

	std::vector<std::string> Strings(const toml::node_view<const toml::node>& node, bool lower)
	{
		std::vector<std::string> out;

		if (const toml::array* list = node.as_array())
		{
			for (const toml::node& item : *list)
			{
				if (auto text = item.value<std::string>())
				{
					std::string value = *text;

					if (lower)
					{
						std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return (char)std::tolower(c); });
					}

					if (!value.empty())
					{
						out.push_back(value);
					}
				}
			}
		}

		return out;
	}
}

const char* GuardDetectionName(int code)
{
	return (code > 0 && code < kDetections) ? kDetectionNames[code] : kDetectionNames[0];
}

GuardSettings ReadGuardSettings(const std::string& dataPath)
{
	std::lock_guard<std::mutex> lock(s_Lock);

	const std::string folder = dataPath + "Guard/";
	const std::string path = folder + "Guard.toml";

	std::error_code error;
	const bool exists = std::filesystem::exists(path, error);
	const auto time = exists ? std::filesystem::last_write_time(path, error) : std::filesystem::file_time_type();

	if (path == s_Path && exists == s_Exists && (!exists || time == s_Time))
	{
		return s_Settings;
	}

	s_Path = path;
	s_Exists = exists;
	s_Time = time;

	GuardSettings settings;
	settings.Socket = folder + "mu-guard.sock";
	settings.WarningCodes = DefaultWarningCodes();

	if (exists)
	{
		toml::parse_result result = toml::parse_file(path);

		// Mal escrito: queda apagado, igual que sin el archivo.
		if (result)
		{
			const toml::table& table = result.table();

			settings.Enabled = table["Enabled"].value_or(false);
			settings.Required = table["Required"].value_or(false);
			settings.MemScanCloses = table["MemScanCloses"].value_or(true);
			settings.AcceptUnknownBuilds = table["AcceptUnknownBuilds"].value_or(true);
			settings.ToolCloses = table["ToolCloses"].value_or(true);

			settings.MaxPerMachine = ClampInt(table["MaxPerMachine"].value_or(std::int64_t(0)), 0, 255);

			settings.FilesCloses = table["FilesCloses"].value_or(true);

			settings.WebClients = table["WebClients"].value_or(true);

			const std::int64_t challenge = table["ChallengeSeconds"].value_or(std::int64_t(30));

			settings.ChallengeSeconds = (int)(challenge < 5 ? 5 : (challenge > 300 ? 300 : challenge));

			const std::int64_t port = table["Port"].value_or(std::int64_t(28980));

			if (port > 0 && port < 65536)
			{
				settings.Port = static_cast<std::uint16_t>(port);
			}

			const std::string socket = table["Socket"].value_or(std::string());

			if (!socket.empty())
			{
				settings.Socket = socket;
			}

			const toml::node_view<const toml::node> warnings = table["warnings"];

			settings.WarningLimit = ClampInt(warnings["limit"].value_or(std::int64_t(3)), 0, 1000);

			settings.WarningWindowDays = ClampInt(warnings["window_days"].value_or(std::int64_t(30)), 1, 3650);

			if (warnings["codes"].as_array() != nullptr)
			{
				settings.WarningCodes = 0;

				for (const std::string& name : Strings(warnings["codes"], false))
				{
					for (int code = 1; code < kDetections; code++)
					{
						std::string upper = name;

						std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return (char)std::toupper(c); });

						if (upper == kDetectionNames[code])
						{
							settings.WarningCodes |= 1u << code;
						}
					}
				}
			}

			const toml::node_view<const toml::node> ban = table["ban"];

			settings.BanAccountDays = ClampInt(ban["account_days"].value_or(std::int64_t(7)), -1, 36500);

			settings.BanMachineDays = ClampInt(ban["machine_days"].value_or(std::int64_t(7)), -1, 36500);

			settings.BanIpHours = ClampInt(ban["ip_hours"].value_or(std::int64_t(24)), -1, 876000);

			settings.ExemptAccounts = Strings(ban["exempt_accounts"], true);

			settings.ExemptIps = Strings(ban["exempt_ips"], false);
		}
	}

	s_Settings = settings;

	return s_Settings;
}

namespace
{
	std::mutex s_RulesLock;
	std::filesystem::file_time_type s_ListsTime, s_MessagesTime, s_FilesTime, s_OverlayTime, s_DefaultsTime;
	bool s_ListsExist = false, s_MessagesExist = false, s_FilesExist = false, s_OverlayExist = false, s_DefaultsExist = false;
	std::string s_RulesPath;
	GuardRules s_Rules;

	// Entradas de una lista: sin vacías, recortadas al tope y en minúsculas.
	void Collect(std::vector<std::string>& out, const toml::node_view<const toml::node>& node)
	{
		for (std::string& value : Strings(node, true))
		{
			if (value.size() > GuardRuleMax)
			{
				value.resize(GuardRuleMax);
			}

			out.push_back(value);
		}
	}
}

GuardRules ReadGuardRules(const std::string& dataPath)
{
	std::lock_guard<std::mutex> lock(s_RulesLock);

	const std::string lists = dataPath + "Guard/Lists.toml";
	const std::string messages = dataPath + "Guard/Messages.toml";
	const std::string files = dataPath + "Guard/Files.txt";
	const std::string overlay = dataPath + "Patches/published/manifest.json";
	const std::string defaults = dataPath + "Guard/Lists.default.toml";

	std::error_code error;
	const bool listsExist = std::filesystem::exists(lists, error);
	const bool messagesExist = std::filesystem::exists(messages, error);
	const auto listsTime = listsExist ? std::filesystem::last_write_time(lists, error) : std::filesystem::file_time_type();
	const auto messagesTime = messagesExist ? std::filesystem::last_write_time(messages, error) : std::filesystem::file_time_type();
	const bool filesExist = std::filesystem::exists(files, error);
	const auto filesTime = filesExist ? std::filesystem::last_write_time(files, error) : std::filesystem::file_time_type();
	const bool overlayExist = std::filesystem::exists(overlay, error);
	const auto overlayTime = overlayExist ? std::filesystem::last_write_time(overlay, error) : std::filesystem::file_time_type();
	const bool defaultsExist = std::filesystem::exists(defaults, error);
	const auto defaultsTime = defaultsExist ? std::filesystem::last_write_time(defaults, error) : std::filesystem::file_time_type();

	if (s_RulesPath == dataPath && s_Rules.Version != 0 && listsExist == s_ListsExist && messagesExist == s_MessagesExist &&
		listsTime == s_ListsTime && messagesTime == s_MessagesTime && filesExist == s_FilesExist && filesTime == s_FilesTime &&
		overlayExist == s_OverlayExist && overlayTime == s_OverlayTime && defaultsExist == s_DefaultsExist && defaultsTime == s_DefaultsTime)
	{
		return s_Rules;
	}

	GuardRules rules;
	rules.Version = s_Rules.Version + 1;

	// Las de fábrica (Lists.default.toml, las reescribe el arranque en cada
	// versión) y las del operador (Lists.toml) se suman.
	for (const std::string* file : { &defaults, &lists })
	{
		if (!std::filesystem::exists(*file, error))
		{
			continue;
		}

		toml::parse_result result = toml::parse_file(*file);

		if (result)
		{
			const toml::table& table = result.table();

			Collect(rules.Processes, table["tool"]["processes"]);
			Collect(rules.Windows, table["tool"]["windows"]);
			Collect(rules.ModuleBlock, table["modules"]["block"]);
			Collect(rules.ModuleAllow, table["modules"]["allow"]);
		}
	}

	for (std::vector<std::string>* list : { &rules.Processes, &rules.Windows, &rules.ModuleBlock, &rules.ModuleAllow })
	{
		std::sort(list->begin(), list->end());

		list->erase(std::unique(list->begin(), list->end()), list->end());
	}

	if (messagesExist)
	{
		toml::parse_result result = toml::parse_file(messages);

		if (result)
		{
			for (const char* language : { "es", "en", "pt" })
			{
				const toml::table* names = result.table()[language].as_table();

				if (names == nullptr)
				{
					continue;
				}

				for (const auto& entry : *names)
				{
					auto text = entry.second.value<std::string>();

					if (!text || text->empty())
					{
						continue;
					}

					std::string code(entry.first.str());

					std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return (char)std::toupper(c); });

					std::string line = std::string(language) + "\t" + code + "\t" + *text;

					if (line.size() <= GuardRuleMax)
					{
						rules.Names.push_back(line);
					}
				}
			}
		}
	}

	// Files.txt: "hash  ruta" por línea (lo escribe mu-guard files), la base del
	// cliente.  Encima, lo que cambió el overlay publicado: así un parche nuevo
	// no obliga a regenerar Files.txt.
	std::map<std::string, std::string> tracked;

	if (filesExist)
	{
		std::ifstream in(files);

		std::string line;

		while (std::getline(in, line))
		{
			if (!line.empty() && line.back() == '\r')
			{
				line.pop_back();
			}

			const std::size_t gap = line.find_first_of(" \t");

			if (gap != 64 || line.find_first_not_of("0123456789abcdef") < 64)
			{
				continue;
			}

			const std::size_t path = line.find_first_not_of(" \t", gap);

			if (path == std::string::npos)
			{
				continue;
			}

			tracked[line.substr(path)] = line.substr(0, 64);
		}

		if (overlayExist)
		{
			ReadOverlay(overlay, tracked);
		}
	}

	for (const auto& entry : tracked)
	{
		const std::string rule = entry.second + "\t" + entry.first;

		if (rule.size() <= GuardRuleMax)
		{
			rules.Files.push_back(rule);
		}
	}

	s_RulesPath = dataPath;
	s_FilesExist = filesExist;
	s_FilesTime = filesTime;
	s_OverlayExist = overlayExist;
	s_OverlayTime = overlayTime;
	s_DefaultsExist = defaultsExist;
	s_DefaultsTime = defaultsTime;
	s_ListsExist = listsExist;
	s_MessagesExist = messagesExist;
	s_ListsTime = listsTime;
	s_MessagesTime = messagesTime;
	s_Rules = rules;

	return s_Rules;
}
