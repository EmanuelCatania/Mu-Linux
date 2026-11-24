// Ver GuardConfig.h.  Sin stdafx.h: lo compilan el ConnectServer, el GameServer
// y mu-guard.

#include "GuardConfig.h"

#define TOML_EXCEPTIONS 0
#include "tomlplusplus/toml.hpp"

#include <filesystem>
#include <mutex>

namespace
{
	std::mutex s_Lock;
	std::string s_Path;
	std::filesystem::file_time_type s_Time;
	bool s_Exists = false;
	GuardSettings s_Settings;
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

	if (exists)
	{
		toml::parse_result result = toml::parse_file(path);

		// Mal escrito: queda apagado, igual que sin el archivo.
		if (result)
		{
			const toml::table& table = result.table();

			settings.Enabled = table["Enabled"].value_or(false);
			settings.Required = table["Required"].value_or(false);

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
		}
	}

	s_Settings = settings;

	return s_Settings;
}
