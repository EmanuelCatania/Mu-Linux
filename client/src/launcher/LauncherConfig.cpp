#include "LauncherConfig.h"

#include "IniDocument.h"

#include "app/config/OptionTable.h"
#include "app/config/Release.h"
#include "update/FileUtil.h"

#include <cctype>
#include <cstdlib>

#ifndef MU_LAUNCHER_TITLE
#define MU_LAUNCHER_TITLE "Mu Linux"
#endif
#ifndef MU_BASE_REPO
#define MU_BASE_REPO "EmanuelCatania/Mu-Linux"
#endif
#ifndef MU_BASE_TAG
#define MU_BASE_TAG MU_RELEASE
#endif
#ifndef MU_BASE_KEY
#define MU_BASE_KEY ""
#endif
#ifndef MU_OVERLAY_URL
#define MU_OVERLAY_URL ""
#endif
#ifndef MU_OVERLAY_CHANNEL
#define MU_OVERLAY_CHANNEL ""
#endif
#ifndef MU_OVERLAY_KEY
#define MU_OVERLAY_KEY ""
#endif
#ifndef MU_GAME_EXE
#ifdef _WIN32
#define MU_GAME_EXE "main.exe"
#else
#define MU_GAME_EXE "main"
#endif
#endif

namespace Launcher {

namespace {

std::string Trim(const std::string& s)
{
	std::size_t a = 0, b = s.size();
	while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
	while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
	return s.substr(a, b - a);
}

// Valor TOML de una línea: string entre comillas, booleano o entero.
bool ParseValue(const std::string& raw, std::string& out)
{
	std::string v = Trim(raw);
	if (!v.empty() && v[0] == '"')
	{
		const std::size_t end = v.find('"', 1);
		if (end == std::string::npos) return false;
		const std::string rest = Trim(v.substr(end + 1));
		if (!rest.empty() && rest[0] != '#') return false;
		out = v.substr(1, end - 1);
		return true;
	}
	const std::size_t hash = v.find('#');
	if (hash != std::string::npos) v = Trim(v.substr(0, hash));
	if (v.empty()) return false;
	out = v;
	return true;
}

bool ParseBool(const std::string& v, bool& out)
{
	if (v == "true") { out = true; return true; }
	if (v == "false") { out = false; return true; }
	return false;
}

bool ParseInt(const std::string& v, long long lo, long long hi, long long& out)
{
	if (v.empty()) return false;
	char* end = nullptr;
	const long long n = std::strtoll(v.c_str(), &end, 10);
	if (!end || *end || n < lo || n > hi) return false;
	out = n;
	return true;
}

} // namespace

Config CompiledConfig()
{
	Config c;
	c.title = MU_LAUNCHER_TITLE;
	c.baseRepo = MU_BASE_REPO;
	c.baseTag = MU_BASE_TAG;
	c.baseKey = MU_BASE_KEY;
	c.overlayUrl = MU_OVERLAY_URL;
	c.overlayChannel = MU_OVERLAY_CHANNEL;
	c.overlayKey = MU_OVERLAY_KEY;
	c.gameExe = MU_GAME_EXE;
	return c;
}

void ApplyLauncherToml(const std::string& path, Config& config, std::vector<std::string>* warnings)
{
	std::string text;
	if (!Update::FileExists(path) || !Update::ReadWholeFile(path, text)) return;
	auto warn = [&](const std::string& w) {
		if (warnings) warnings->push_back(path + ": " + w);
	};
	std::size_t pos = 0;
	int lineNo = 0;
	while (pos <= text.size())
	{
		std::size_t nl = text.find('\n', pos);
		if (nl == std::string::npos) nl = text.size();
		const std::string line = Trim(text.substr(pos, nl - pos));
		pos = nl + 1;
		++lineNo;
		if (line.empty() || line[0] == '#') continue;
		const std::size_t eq = line.find('=');
		std::string value;
		if (eq == std::string::npos || !ParseValue(line.substr(eq + 1), value))
		{
			warn("línea " + std::to_string(lineNo) + " no se entiende");
			continue;
		}
		const std::string key = Trim(line.substr(0, eq));
		long long n = 0;
		bool b = false;
		if (key == "Music" && ParseBool(value, b)) config.music = b;
		else if (key == "HD" && ParseBool(value, b))
		{
			config.hd = b;
			config.hdSet = true;
		}
		else if (key == "Connections" && ParseInt(value, 1, 16, n)) config.connections = static_cast<int>(n);
		else if (key == "SpeedLimitKB" && ParseInt(value, 0, 10000000, n)) config.speedLimit = static_cast<std::uint64_t>(n) * 1024;
		else if (key == "Scale" && ParseInt(value, 0, 300, n) && (n == 0 || n >= 100)) config.scale = static_cast<int>(n);
		else if (key == "BaseSource")
		{
			if (value.find("://") != std::string::npos)
			{
				config.baseUrl = value;
				config.baseRepo.clear();
			}
			else
			{
				config.baseRepo = value;
				config.baseUrl.clear();
			}
			config.overrides.push_back("origen de la base: " + value);
		}
		else if (key == "BaseTag")
		{
			config.baseTag = value;
			config.overrides.push_back("tag de la base: " + value);
		}
		else if (key == "BaseKey")
		{
			config.baseKey = value;
			config.overrides.push_back("clave de la base cambiada");
		}
		else
			warn("clave o valor no válido en la línea " + std::to_string(lineNo) + " (" + key + ")");
	}
}

bool HDTexturesEnabled(const std::string& configIni)
{
	IniDocument ini;
	std::string v;
	const auto& o = UserOptionTable::kHDTextures;
	return ini.Load(configIni) && ini.Get(o.section, o.key, v) && v == "1";
}

std::string UserDirFor(const std::string& root)
{
#ifdef _WIN32
	return Update::JoinPath(root, "User");
#else
	return root;
#endif
}

} // namespace Launcher
