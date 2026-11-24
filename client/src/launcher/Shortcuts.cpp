#include "Shortcuts.h"

#include "Lang.h"
#include "Resources.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#endif

namespace Launcher {

namespace {

namespace fs = std::filesystem;

#ifdef _WIN32

// Un nombre de archivo a partir del título.
std::string FileNameOf(const std::string& name)
{
	std::string out;
	for (char c : name)
		out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
		        static_cast<unsigned char>(c) < 32)
		           ? '_'
		           : c;
	while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
	return out.empty() ? std::string("Mu Online") : out;
}

bool MakeLink(const fs::path& target, const fs::path& link)
{
	IShellLinkW* shell = nullptr;
	if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&shell)))) return false;
	shell->SetPath(target.c_str());
	shell->SetWorkingDirectory(target.parent_path().c_str());
	shell->SetIconLocation(target.c_str(), 0);
	IPersistFile* file = nullptr;
	bool ok = SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&file)));
	if (ok)
	{
		ok = SUCCEEDED(file->Save(link.c_str(), TRUE));
		file->Release();
	}
	shell->Release();
	return ok;
}

#else

// Para que dos launchers (de servers distintos) no se pisen.
std::string IdOf(const std::string& exe)
{
	std::uint32_t h = 2166136261u;
	for (unsigned char c : exe) h = (h ^ c) * 16777619u;
	static const char hex[] = "0123456789abcdef";
	std::string id = "mu-launcher-";
	for (int i = 7; i >= 0; --i) id += hex[(h >> (i * 4)) & 15];
	return id;
}

// Una ruta para Exec= (especificación de .desktop): entre comillas, con \ " ` $ escapados.
std::string Quoted(const std::string& s)
{
	std::string out = "\"";
	for (char c : s)
	{
		if (c == '"' || c == '`' || c == '$' || c == '\\') out += '\\';
		out += c;
	}
	return out + "\"";
}

fs::path DesktopDir(const fs::path& home)
{
	// XDG_DESKTOP_DIR="$HOME/Escritorio" en ~/.config/user-dirs.dirs.
	const char* config = std::getenv("XDG_CONFIG_HOME");
	std::ifstream in((config && *config ? fs::path(config) : home / ".config") / "user-dirs.dirs");
	std::string line;
	while (std::getline(in, line))
	{
		const std::string key = "XDG_DESKTOP_DIR=\"";
		if (line.compare(0, key.size(), key) != 0 || line.back() != '"') continue;
		std::string value = line.substr(key.size(), line.size() - key.size() - 1);
		if (value.compare(0, 5, "$HOME") == 0) value = home.string() + value.substr(5);
		return fs::path(value);
	}
	return home / "Desktop";
}

bool WriteFile(const fs::path& path, const std::string& text, fs::perms perms)
{
	std::error_code ec;
	fs::create_directories(path.parent_path(), ec);
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) return false;
		out << text;
		if (!out) return false;
	}
	fs::permissions(path, perms, ec);
	return true;
}

#endif

} // namespace

bool CreateShortcuts(const std::string& exe, const std::string& name, std::string* why)
{
#ifdef _WIN32
	const std::string file = FileNameOf(name);
	const fs::path target = fs::u8path(exe).make_preferred();
	const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	int made = 0;
	for (const KNOWNFOLDERID& folder : { FOLDERID_Desktop, FOLDERID_Programs })
	{
		PWSTR dir = nullptr;
		if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &dir)))
			made += MakeLink(target, fs::path(dir) / fs::u8path(file + ".lnk")) ? 1 : 0;
		CoTaskMemFree(dir);
	}
	if (SUCCEEDED(com)) CoUninitialize();
	if (made == 0 && why) *why = Tr("Windows no dejó crearlos");
	return made > 0;
#else
	const char* homeEnv = std::getenv("HOME");
	if (!homeEnv || !*homeEnv)
	{
		if (why) *why = Tr("falta la variable HOME");
		return false;
	}
	const fs::path home(homeEnv);
	const char* dataEnv = std::getenv("XDG_DATA_HOME");
	const fs::path data = dataEnv && *dataEnv ? fs::path(dataEnv) : home / ".local" / "share";
	const std::string id = IdOf(exe);
	const fs::path icon = data / "icons" / "hicolor" / "48x48" / "apps" / (id + ".png");
	{
		std::error_code ec;
		fs::create_directories(icon.parent_path(), ec);
		std::ofstream out(icon, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(LauncherRes::LauncherIcon), static_cast<std::streamsize>(LauncherRes::LauncherIconSize));
	}
	const std::string entry = "[Desktop Entry]\nType=Application\nName=" + name + "\nExec=" + Quoted(exe) +
	                          "\nPath=" + fs::path(exe).parent_path().string() + "\nIcon=" + icon.string() +
	                          "\nCategories=Game;\nTerminal=false\n";
	const fs::perms readable = fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read;
	// El del escritorio ejecutable: varios escritorios no abren uno que no lo es.
	const fs::perms executable = readable | fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec;
	int made = WriteFile(data / "applications" / (id + ".desktop"), entry, readable) ? 1 : 0;
	const fs::path desktop = DesktopDir(home);
	std::error_code ec;
	if (fs::is_directory(desktop, ec)) made += WriteFile(desktop / (id + ".desktop"), entry, executable) ? 1 : 0;
	if (made == 0 && why) *why = Fmt("no se pudo escribir en {0}", { data.string() });
	return made > 0;
#endif
}

} // namespace Launcher
