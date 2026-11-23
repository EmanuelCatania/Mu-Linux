#pragma once

// Baja y valida los manifiestos de los canales antes de planear nada.
//
// Un manifiesto se acepta sólo si:
// - la firma es de la clave de ese origen (la del Release del proyecto o del
//   fork, o la identidad del server para su overlay);
// - es del canal pedido y, si se pidió una versión, de esa versión (un asset
//   subido al Release equivocado no se acepta);
// - su secuencia no es menor que la más alta ya vista para ese canal: sin eso,
//   alguien podría reenviar un parche viejo con firma válida.
//
// Las secuencias vistas se guardan en User/Cache/sequences.json.

#include "update/InstallPlan.h"
#include "update/Manifest.h"
#include "update/Signing.h"
#include "update/Transfer.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace Update {

constexpr std::uint64_t kMaxManifestSize = 64ull << 20;

struct ChannelSpec
{
	std::string channel;                     // "mu-linux/base"
	// Carpetas o URLs donde está el manifiesto, en orden de preferencia.  Los
	// zips sin url se bajan de la misma que dio el manifiesto, o de las otras.
	std::vector<std::string> urls;
	std::string manifestName = "manifest.json";   // en un Release del proyecto: "<canal>.manifest.json"
	std::uint8_t publicKey[kKeySize]{};
	std::string version;                     // vacío: cualquiera
	int priority = 0;                        // base 0, HD 10, overlay 20 (ver ChannelSource)
};

struct FetchedChannel
{
	Manifest manifest;
	std::vector<std::string> urls;           // la que respondió primero, después el resto
	bool unknownFormat = false;              // de un formato más nuevo: hay que actualizar el launcher
};

class SequenceStore
{
public:
	// Uno que falta queda vacío; uno roto es error (no se puede saber qué se vio).
	bool Load(const std::string& path, std::string* error);
	bool Save(const std::string& path) const;
	std::uint64_t Seen(const std::string& channel) const;
	void Record(const std::string& channel, std::uint64_t sequence);

private:
	std::map<std::string, std::uint64_t> m_Seen;
};

// Baja <url>/<manifestName> y su ".sig" probando las URLs en orden, valida todo
// y registra la secuencia (sin guardar el archivo).
bool FetchChannel(Transport& transport, const ChannelSpec& spec, SequenceStore& sequences,
                  FetchedChannel& out, std::string* error);

// El ChannelSource para planear con lo que se bajó.
ChannelSource SourceOf(const FetchedChannel& fetched, int priority);

// Carpeta de assets de un Release de GitHub: lo que el launcher pide es siempre
// un tag exacto, nunca "el último".
std::string GithubReleaseUrl(const std::string& repo, const std::string& tag);
// "mu-linux/base" -> "mu-linux-base.manifest.json" (los assets de un Release
// comparten carpeta).
std::string ReleaseManifestName(const std::string& channel);

// El overlay de un server pide una base y un protocolo; false si no coinciden.
bool CheckRequires(const Manifest& base, const Manifest& overlay, int protocol, std::string* error);

} // namespace Update
