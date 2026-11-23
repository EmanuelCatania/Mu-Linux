#pragma once

// mu-patch publish / watch: arma el canal del operador desde Data/Patches/
// (diseño, 2.4).
//
//   Data/Patches/
//   ├── 19112026-1/update.zip        el admin sube un zip ya armado (cualquier
//   ├── 19112026-2/main.exe          herramienta: Deflate o sin comprimir)...
//   │             /Content/item/...  ...o los archivos sueltos
//   │             /deleted.txt       opcional: rutas a borrar del cliente
//   └── published/                   lo genera publish: es lo que se sirve
//       ├── manifest.json + .sig
//       ├── <canal>-<versión>-N.zip
//       └── history/<secuencia>/     manifiestos anteriores (para los deltas)
//
// - Carpetas "DDMMAAAA-N", ordenadas por fecha y número (no alfabético: 19112026
//   es anterior a 01122026).  Un nombre que no cumple se ignora con un aviso.
// - El manifiesto es el estado final de todos los parches: el más nuevo gana
//   cada ruta; deleted.txt saca una ruta (y otro parche posterior la puede
//   volver a traer).  Un jugador que se salteó parches baja sólo lo último.
// - Los zips de published/ que siguen sirviendo se reusan; lo que cambió va en
//   zips nuevos, con delta contra las 3 versiones anteriores.  Se borran los
//   zips que ya no usa ninguna de esas versiones.
// - Si nada cambió no publica una versión nueva.
// - Se firma con la identidad del server (Data/Keys/ServerIdentity.key): la
//   clave privada no sale del server.

#include "PatchBuilder.h"

#include "update/Announce.h"

#include <cstdint>
#include <string>
#include <vector>

namespace MuPatch {

struct PublishOptions
{
	std::string patches;          // Data/Patches
	std::string out;              // vacío: <patches>/published
	std::string channel;          // "<server>/overlay"
	std::string keyPath;
	std::string base;             // requires.base (vacío: sale del ejecutable)
	int protocol = 0;
	int level = 19;
	int threads = 0;
	Log log;
};

// *published: true si salió una versión nueva.
bool PublishPatches(const PublishOptions& options, bool* published, std::string* error);

// launcher.json (update/Announce.h): la configuración del server para el
// launcher, firmada con la misma identidad.  Va en la misma carpeta que el
// manifiesto (la sirven mu-web y el ConnectServer).
struct AnnounceOptions
{
	std::string out;              // <patches>/published
	std::string keyPath;
	Update::Announce content;     // la secuencia la pone PublishAnnounce
	Log log;
};

// Publica sólo si cambió algo; la secuencia sigue a la del archivo anterior,
// aunque esté firmado con una clave que ya rotó.  *published: true si escribió.
bool PublishAnnounce(const AnnounceOptions& options, bool* published, std::string* error);

// Nombre de carpeta de parche válido; *order queda con una clave ordenable.
bool ParsePatchFolder(const std::string& name, std::uint64_t* order);

// Lo que hay en Data/Patches (sin published/): ruta, tamaño y fecha de cada
// archivo.  watch publica cuando cambia y se mantiene igual entre dos pasadas
// (una carpeta a medio subir cambia entre pasadas).
std::string SnapshotPatches(const std::string& patches, const std::string& out);

} // namespace MuPatch
