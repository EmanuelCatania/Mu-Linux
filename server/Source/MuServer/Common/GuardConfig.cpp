// Ver GuardConfig.h.  Sin stdafx.h: lo compilan el ConnectServer, el GameServer
// y mu-guard.

#include "GuardConfig.h"

#define TOML_EXCEPTIONS 0
#include "tomlplusplus/toml.hpp"

#include <algorithm>
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

	const char* const kDetectionNames[] = { "?", "DEBUGGER", "INJECT", "MEMEDIT", "MEMSCAN", "PATCH", "FILES", "TOOL" };

	constexpr int kDetections = (int)(sizeof(kDetectionNames) / sizeof(kDetectionNames[0]));

	std::uint32_t DefaultWarningCodes()
	{
		return (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5);
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
