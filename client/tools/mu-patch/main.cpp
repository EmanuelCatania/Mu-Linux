// mu-patch: arma y verifica los canales de parches del launcher.
//
//   mu-patch keygen     --out <archivo>
//   mu-patch pubkey     --key <archivo>
//   mu-patch release-of <ejecutable>
//   mu-patch build      --channel <dueño/canal> --version <x.y.z> --in <carpeta> --out <carpeta> --key <archivo>
//                       [--base <x.y.z>] [--protocol <n>] [--sequence <n>]
//                       [--prev <carpeta>[@<url>]]... [--include <glob>]... [--exclude <glob>]...
//                       [--optional <glob>]... [--deleted <archivo>] [--base-manifest <archivo>]
//                       [--bundle-size <MB>] [--level <1-22>] [--threads <n>]
//   mu-patch verify     --in <carpeta> (--pubkey <hex> | --key <archivo>) [--prev <carpeta>]...
//   mu-patch publish    --patches <Data/Patches> --channel <server/overlay> --key <archivo>
//                       [--out <carpeta>] [--base <x.y.z>] [--protocol <n>] [--level <1-22>] [--threads <n>]
//   mu-patch watch      (las mismas opciones) [--interval <segundos>]

#include "PatchBuilder.h"
#include "Publish.h"

#include "update/FileUtil.h"
#include "update/Hash.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

const char kUsage[] =
	"mu-patch: parches del cliente Mu-Linux\n"
	"\n"
	"  keygen     --out <archivo>          crea una clave de firma (canales del proyecto)\n"
	"  pubkey     --key <archivo>          muestra la clave pública\n"
	"  release-of <ejecutable>             muestra la versión (MU_RELEASE) de un cliente\n"
	"  build      --channel <dueño/canal> --version <x.y.z> --in <carpeta> --out <carpeta> --key <archivo>\n"
	"             [--base <x.y.z>] [--protocol <n>] [--sequence <n>]\n"
	"             [--prev <carpeta>[@<url>]]... [--include <glob>]... [--exclude <glob>]...\n"
	"             [--optional <glob>]... [--deleted <archivo>] [--base-manifest <archivo>]\n"
	"             [--bundle-size <MB>] [--level <1-22>] [--threads <n>]\n"
	"  verify     --in <carpeta> (--pubkey <hex> | --key <archivo>) [--prev <carpeta>]...\n"
	"  publish    --patches <Data/Patches> --channel <server/overlay> --key <archivo>\n"
	"             [--out <carpeta>] [--base <x.y.z>] [--protocol <n>] [--level <1-22>] [--threads <n>]\n"
	"             arma el canal desde las carpetas DDMMAAAA-N (por defecto en <patches>/published)\n"
	"  watch      las opciones de publish y [--interval <segundos>]: publica cada vez que\n"
	"             Data/Patches cambia y se queda quieto entre dos pasadas\n"
	"\n"
	"El overlay de un server se firma con su Data/Keys/ServerIdentity.key.\n";

struct Args
{
	std::vector<std::string> items;
	std::size_t pos = 0;
	bool HasMore() const { return pos < items.size(); }
	const std::string& Next() { return items[pos++]; }
};

void Print(const std::string& s)
{
	std::fputs(s.c_str(), stdout);
	std::fputc('\n', stdout);
	std::fflush(stdout);
}

int Error(const std::string& s)
{
	std::fputs(("mu-patch: " + s + "\n").c_str(), stderr);
	return 1;
}

bool ParseUint(const std::string& s, unsigned long long& out)
{
	if (s.empty()) return false;
	char* end = nullptr;
	out = std::strtoull(s.c_str(), &end, 10);
	return end && *end == 0 && s[0] != '-';
}

MuPatch::PrevSource ParsePrev(const std::string& value)
{
	MuPatch::PrevSource p;
	const std::size_t at = value.rfind('@');
	if (at != std::string::npos && (value.compare(at + 1, 7, "http://") == 0 || value.compare(at + 1, 8, "https://") == 0))
	{
		p.dir = value.substr(0, at);
		p.url = value.substr(at + 1);
	}
	else
	{
		p.dir = value;
	}
	return p;
}

bool ReadLines(const std::string& path, std::vector<std::string>& out)
{
	std::string text;
	if (!Update::ReadWholeFile(path, text)) return false;
	std::size_t start = 0;
	while (start < text.size())
	{
		std::size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(start, end - start);
		while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
		if (!line.empty() && line[0] != '#') out.push_back(line);
		start = end + 1;
	}
	return true;
}

int CmdKeygen(Args& a)
{
	std::string out;
	while (a.HasMore())
	{
		const std::string opt = a.Next();
		if (opt == "--out" && a.HasMore()) out = a.Next();
		else return Error("opción desconocida: " + opt);
	}
	if (out.empty()) return Error("falta --out");
	if (Update::FileExists(out)) return Error(out + " ya existe: no se pisa una clave");
	Update::KeyPair key;
	if (!Update::GenerateKey(key)) return Error("el sistema no dio bytes aleatorios");
	const bool ok = Update::SaveKeyFile(out, key);
	const std::string pub = Update::ToHex(key.publicKey, Update::kKeySize);
	key.Wipe();
	if (!ok) return Error("no se puede escribir " + out);
	Print("clave creada en " + out);
	Print("pública: " + pub);
	return 0;
}

int CmdPubkey(Args& a)
{
	std::string path;
	while (a.HasMore())
	{
		const std::string opt = a.Next();
		if (opt == "--key" && a.HasMore()) path = a.Next();
		else return Error("opción desconocida: " + opt);
	}
	if (path.empty()) return Error("falta --key");
	Update::KeyPair key;
	std::string error;
	if (!Update::LoadKeyFile(path, key, &error)) return Error(error);
	Print(Update::ToHex(key.publicKey, Update::kKeySize));
	key.Wipe();
	return 0;
}

int CmdReleaseOf(Args& a)
{
	if (!a.HasMore()) return Error("falta el ejecutable");
	const std::string path = a.Next();
	std::vector<std::uint8_t> data;
	if (!Update::ReadWholeFile(path, data)) return Error("no se puede leer " + path);
	std::string version;
	const std::string platform = MuPatch::DetectPlatform(data);
	if (!MuPatch::ReadReleaseMarker(data, version)) return Error(path + " no tiene MU_RELEASE" + (platform.empty() ? "" : " (" + platform + ")"));
	Print(version + (platform.empty() ? "" : " " + platform));
	return 0;
}

int CmdBuild(Args& a)
{
	MuPatch::BuildOptions o;
	o.log = Print;
	std::string deletedFile;
	while (a.HasMore())
	{
		const std::string opt = a.Next();
		if (!a.HasMore()) return Error("falta el valor de " + opt);
		const std::string v = a.Next();
		unsigned long long n = 0;
		if (opt == "--channel") o.channel = v;
		else if (opt == "--version") o.version = v;
		else if (opt == "--base") o.base = v;
		else if (opt == "--in") o.in = v;
		else if (opt == "--out") o.out = v;
		else if (opt == "--key") o.keyPath = v;
		else if (opt == "--prev") o.prev.push_back(ParsePrev(v));
		else if (opt == "--include") o.include.push_back(v);
		else if (opt == "--exclude") o.exclude.push_back(v);
		else if (opt == "--optional") o.optional.push_back(v);
		else if (opt == "--deleted") deletedFile = v;
		else if (opt == "--base-manifest") o.baseManifest = v;
		else if (opt == "--protocol") { if (!ParseUint(v, n) || n > 65535) return Error("--protocol no válido"); o.protocol = static_cast<int>(n); }
		else if (opt == "--sequence") { if (!ParseUint(v, n) || n == 0) return Error("--sequence no válido"); o.sequence = n; }
		else if (opt == "--bundle-size") { if (!ParseUint(v, n) || n == 0 || n > 4096) return Error("--bundle-size no válido (MB)"); o.bundleSize = n << 20; }
		else if (opt == "--level") { if (!ParseUint(v, n) || n > 22) return Error("--level no válido"); o.level = static_cast<int>(n); }
		else if (opt == "--threads") { if (!ParseUint(v, n) || n > 256) return Error("--threads no válido"); o.threads = static_cast<int>(n); }
		else return Error("opción desconocida: " + opt);
	}
	if (o.channel.empty() || o.version.empty() || o.in.empty() || o.out.empty() || o.keyPath.empty())
		return Error("build necesita --channel, --version, --in, --out y --key");
	if (!deletedFile.empty() && !ReadLines(deletedFile, o.deleted)) return Error("no se puede leer " + deletedFile);
	std::string error;
	if (!MuPatch::BuildPatch(o, &error)) return Error(error);
	return 0;
}

// publish y watch comparten opciones.
int ParsePublish(Args& a, MuPatch::PublishOptions& o, unsigned* interval)
{
	o.log = Print;
	while (a.HasMore())
	{
		const std::string opt = a.Next();
		if (!a.HasMore()) return Error("falta el valor de " + opt);
		const std::string v = a.Next();
		unsigned long long n = 0;
		if (opt == "--patches") o.patches = v;
		else if (opt == "--out") o.out = v;
		else if (opt == "--channel") o.channel = v;
		else if (opt == "--key") o.keyPath = v;
		else if (opt == "--base") o.base = v;
		else if (opt == "--protocol") { if (!ParseUint(v, n) || n > 65535) return Error("--protocol no válido"); o.protocol = static_cast<int>(n); }
		else if (opt == "--level") { if (!ParseUint(v, n) || n > 22) return Error("--level no válido"); o.level = static_cast<int>(n); }
		else if (opt == "--threads") { if (!ParseUint(v, n) || n > 256) return Error("--threads no válido"); o.threads = static_cast<int>(n); }
		else if (opt == "--interval" && interval) { if (!ParseUint(v, n) || n == 0 || n > 86400) return Error("--interval no válido"); *interval = static_cast<unsigned>(n); }
		else return Error("opción desconocida: " + opt);
	}
	if (o.patches.empty() || o.channel.empty() || o.keyPath.empty())
		return Error("necesita --patches, --channel y --key");
	return 0;
}

int CmdPublish(Args& a)
{
	MuPatch::PublishOptions o;
	if (const int rc = ParsePublish(a, o, nullptr)) return rc;
	std::string error;
	if (!MuPatch::PublishPatches(o, nullptr, &error)) return Error(error);
	return 0;
}

// Proceso del server (MU_UPDATE_ENABLED=1): no termina.  Publica al arrancar
// si hace falta y después cada vez que Data/Patches cambia y se queda quieto
// entre dos pasadas (una carpeta a medio subir cambia entre pasadas).  Un
// error no lo cierra: lo avisa y vuelve a intentar con el próximo cambio.
int CmdWatch(Args& a)
{
	MuPatch::PublishOptions o;
	unsigned interval = 30;
	if (const int rc = ParsePublish(a, o, &interval)) return rc;
	const std::string out = o.out.empty() ? Update::JoinPath(o.patches, "published") : o.out;
	std::string done;        // lo que había en la última publicación
	std::string previous;    // lo de la pasada anterior
	for (;;)
	{
		const std::string now = MuPatch::SnapshotPatches(o.patches, out);
		if (now != done && now == previous && !now.empty())
		{
			std::string error;
			if (MuPatch::PublishPatches(o, nullptr, &error)) done = now;
			else
			{
				Print("ERROR: " + error);
				done = now;   // no reintentar hasta que algo cambie
			}
		}
		previous = now;
		std::this_thread::sleep_for(std::chrono::seconds(interval));
	}
}

int CmdVerify(Args& a)
{
	MuPatch::VerifyOptions o;
	o.log = Print;
	bool hasKey = false;
	while (a.HasMore())
	{
		const std::string opt = a.Next();
		if (!a.HasMore()) return Error("falta el valor de " + opt);
		const std::string v = a.Next();
		if (opt == "--in") o.in = v;
		else if (opt == "--prev") o.prev.push_back(ParsePrev(v));
		else if (opt == "--pubkey")
		{
			if (!Update::ParsePublicKey(v, o.publicKey)) return Error("--pubkey tiene que ser 64 dígitos hex");
			hasKey = true;
		}
		else if (opt == "--key")
		{
			Update::KeyPair key;
			std::string error;
			if (!Update::LoadKeyFile(v, key, &error)) return Error(error);
			std::copy(key.publicKey, key.publicKey + Update::kKeySize, o.publicKey);
			key.Wipe();
			hasKey = true;
		}
		else return Error("opción desconocida: " + opt);
	}
	if (o.in.empty() || !hasKey) return Error("verify necesita --in y --pubkey (o --key)");
	std::string error;
	if (!MuPatch::VerifyPatch(o, &error)) return Error(error);
	return 0;
}

std::vector<std::string> CommandLine(int argc, char** argv)
{
	std::vector<std::string> out;
#ifdef _WIN32
	// argv de Windows viene en la página de códigos del sistema: con rutas que no
	// son ASCII se pierden caracteres.  Se toma la línea en UTF-16.
	(void)argc;
	(void)argv;
	int n = 0;
	LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
	for (int i = 1; w && i < n; ++i)
	{
		const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
		std::string s(len > 0 ? static_cast<std::size_t>(len - 1) : 0, '\0');
		if (len > 1) WideCharToMultiByte(CP_UTF8, 0, w[i], -1, &s[0], len, nullptr, nullptr);
		out.push_back(s);
	}
	if (w) LocalFree(w);
#else
	for (int i = 1; i < argc; ++i) out.push_back(argv[i]);
#endif
	return out;
}

} // namespace

int main(int argc, char** argv)
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8);
#endif
	Args a;
	a.items = CommandLine(argc, argv);
	if (!a.HasMore() || a.items[0] == "--help" || a.items[0] == "-h")
	{
		std::fputs(kUsage, stdout);
		return a.HasMore() ? 0 : 1;
	}
	const std::string cmd = a.Next();
	if (cmd == "keygen") return CmdKeygen(a);
	if (cmd == "pubkey") return CmdPubkey(a);
	if (cmd == "release-of") return CmdReleaseOf(a);
	if (cmd == "build") return CmdBuild(a);
	if (cmd == "verify") return CmdVerify(a);
	if (cmd == "publish") return CmdPublish(a);
	if (cmd == "watch") return CmdWatch(a);
	std::fputs(kUsage, stderr);
	return Error("comando desconocido: " + cmd);
}
