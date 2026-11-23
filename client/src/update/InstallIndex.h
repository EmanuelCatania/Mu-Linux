#pragma once

// Índice local de lo que instaló el launcher (User/Cache/install.json).
//
// Guarda por archivo su tamaño, fecha de modificación y hash.  Al comparar contra
// un manifiesto sólo se rehashean los archivos cuyo tamaño o fecha cambiaron;
// "Verificar archivos" rehashea todos.  También sirve para borrar lo que el
// launcher instaló y ya no está en ningún canal, sin tocar nunca un archivo que
// no puso él (los mods del jugador, sus capturas).
//
// "revision" crece cada vez que el launcher cambia algo: el cliente la mira al
// cambiar de mapa para refrescar su índice de contenido (diseño, 4b).

#include "update/Hash.h"

#include <cstdint>
#include <map>
#include <string>

namespace Update {

struct IndexEntry
{
	std::uint64_t size = 0;
	std::int64_t mtime = 0;
	Digest hash{};
	std::string channel;     // canal del que vino
};

struct InstallIndex
{
	std::uint64_t revision = 0;
	// Cuándo se guardó, en las unidades del reloj de archivos.  Una entrada cuya
	// fecha queda cerca de ésta no es confiable (la fecha de un archivo tiene
	// granularidad gruesa: reescrito enseguida podría quedar igual) y se
	// rehashea, como hace git con su índice.  0: nunca se guardó.
	std::int64_t savedAt = 0;
	std::map<std::string, IndexEntry> files;   // por ruta relativa, con "/"
};

// Un índice que falta no es error (instalación nueva: queda vacío).  Uno roto sí:
// el launcher lo descarta y verifica todo.
bool LoadIndex(const std::string& path, InstallIndex& out, std::string* error);
bool SaveIndex(const std::string& path, InstallIndex& index);   // actualiza savedAt

enum class LocalState
{
	Missing,     // no está en disco
	Match,       // tiene el hash esperado
	Differs,     // está, con otro contenido (*actual dice cuál)
	Unreadable,  // está pero no se puede leer
};

// Estado de root/rel contra el hash esperado.  Confía en el índice si el tamaño y
// la fecha coinciden con lo guardado y la fecha es bien anterior al guardado del
// índice (salvo fullVerify); si no, rehashea y actualiza la entrada.  *actual
// queda con el hash del archivo en disco cuando se conoce (sirve para elegir un
// delta).
LocalState CheckLocal(const std::string& root, const std::string& rel, const Digest& expected,
                      InstallIndex& index, bool fullVerify, Digest* actual);

} // namespace Update
