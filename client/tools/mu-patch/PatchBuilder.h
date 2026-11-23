#pragma once

// Lógica de mu-patch (armar y verificar canales de parches), aparte del main
// para poder probarla.  Ver doc del formato en src/update/Manifest.h.

#include "update/Signing.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace MuPatch {

using Log = std::function<void(const std::string&)>;

// Una versión anterior del mismo canal: su carpeta (manifest.json, .sig y los
// zips) y, opcional, la URL desde la que se sirven esos zips.  Lo que no cambió
// se sigue bajando de ahí; los archivos que cambiaron llevan un delta contra ella.
struct PrevSource
{
	std::string dir;
	std::string url;
	std::string zipDir;   // vacío: los zips están en dir
};

struct BuildOptions
{
	std::string channel;            // "mu-linux/base", "<server>/overlay"...
	std::string version;
	std::string base;               // requires.base (overlay: si falta, sale del ejecutable)
	int protocol = 0;               // requires.protocol
	std::uint64_t sequence = 0;     // 0: la siguiente a la de las versiones anteriores
	std::string in;
	std::string out;
	std::string keyPath;
	std::vector<PrevSource> prev;
	std::vector<std::string> include;   // globs; vacío = todo
	std::vector<std::string> exclude;
	std::vector<std::string> optional;  // globs de archivos que no hacen falta para jugar
	std::vector<std::string> deleted;
	std::string baseManifest;           // overlay: para avisar qué archivos de la base pisa
	std::uint64_t bundleSize = 64ull << 20;
	// Los zips que se reusan sin url ya están donde se va a servir el manifiesto
	// nuevo: no se copian a out (publish deja todo en una carpeta).
	bool reuseInPlace = false;
	int level = 19;
	int threads = 0;                    // 0: los núcleos del equipo
	Log log;
};

bool BuildPatch(const BuildOptions& options, std::string* error);

struct VerifyOptions
{
	std::string in;
	std::uint8_t publicKey[Update::kKeySize]{};
	std::vector<PrevSource> prev;       // para comprobar que los deltas dan el archivo nuevo
	Log log;
};

bool VerifyPatch(const VerifyOptions& options, std::string* error);

// "windows-x64", "windows-x86", "linux-x64", "linux-x86", o vacío si el archivo
// no es un ejecutable PE / ELF.
std::string DetectPlatform(const std::vector<std::uint8_t>& data);

// Versión que el cliente lleva compilada (MU_RELEASE), en la marca
// "MuRelease1:<versión>".  false si no la tiene.
bool ReadReleaseMarker(const std::vector<std::uint8_t>& data, std::string& version);

// Globs sobre rutas con "/": "*" y "?" no cruzan carpetas, "**" sí.
bool GlobMatch(const std::string& pattern, const std::string& path);

} // namespace MuPatch
