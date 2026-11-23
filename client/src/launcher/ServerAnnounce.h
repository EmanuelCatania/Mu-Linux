#pragma once

// Qué launcher.json vale (update/Announce.h).
//
// Se confía en la identidad del server: la raíz compilada (MU_OVERLAY_KEY) más
// la cadena de rotaciones, el mismo archivo que usa el cliente
// (User/Cache/server-<raíz>.chain, net/ServerTrust.h).  En orden:
//   1. el guardado en User/Cache/launcher.json, si la firma verifica;
//   2. el de cada ConnectServer (los guardados primero, después los compilados);
//   3. si ninguno contesta, el de /updates/ de cada URL de parches.
// Uno nuevo vale sólo con una secuencia igual o mayor que la del guardado, y
// pasa a ser el guardado.  Si nada responde se usa el guardado.

#include "CsClient.h"

#include "update/Announce.h"
#include "update/Transfer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Launcher {

struct AnnounceSeed
{
	std::string rootKey;                     // hex; vacío: no hay en quién confiar
	std::vector<std::string> connectServers; // "host:puerto" compilados
	std::vector<std::string> urls;           // URL compilada de los parches
};

struct AnnounceResult
{
	bool have = false;               // hay una configuración válida (nueva o guardada)
	bool fresh = false;              // se pudo pedir al server en este arranque
	Update::Announce data;
	std::uint8_t identity[32] = {};  // la clave vigente del server
	bool haveIdentity = false;
	std::vector<std::string> notes;  // por qué falló cada intento
};

// cacheDir: User/Cache.
AnnounceResult ResolveAnnounce(const AnnounceSeed& seed, const std::string& cacheDir, CsFetcher& cs,
                               Update::Transport& http, const std::atomic<bool>* cancel = nullptr);

// "a,b , c" -> {"a","b","c"}.
std::vector<std::string> SplitList(const std::string& s);

} // namespace Launcher
