#pragma once

// Auto-actualización del launcher (diseño, 3.1).
//
// El launcher del operador viaja en su overlay en Launcher/ (mu-patch marca
// "self" a los ejecutables de esa carpeta) y va antes que todo lo demás: si hay
// uno nuevo se instala sólo él, el launcher se reinicia y el nuevo procesa el
// resto.  Así un cambio de formato lo resuelve siempre la versión que lo
// entiende.
//
// Reemplazo: el instalador renombra el que corre a "<exe>.old" (Windows lo
// permite con el archivo abierto; en Linux el proceso sigue con el viejo) y mueve
// el nuevo a su lugar.
//
// Vuelta atrás: antes de arrancar el nuevo se deja una marca.  El nuevo la toma
// al empezar y la borra (junto con el .old) después de unos segundos andando.
// Si al arrancar encuentra una marca ya tomada, el arranque anterior se cayó:
// restaura el .old, deja el nuevo como "<exe>.bad", anota su hash para no
// volver a instalarlo (el viejo vería la misma versión en el overlay y entraría
// en un bucle) y arranca el viejo.  Cuando el operador publica otra versión, el
// hash cambia y se instala normalmente.  No cubre un ejecutable que se cae
// antes de llegar a esa comprobación (la primera cosa que hace main).

#include "update/Hash.h"

#include <string>

namespace Launcher {

struct SelfPaths
{
	std::string exe;        // el launcher instalado
	std::string old;        // la versión anterior, para volver atrás
	std::string bad;        // la que no arrancó
	std::string pending;    // la marca
	std::string blocked;    // hash de una versión que no arrancó: no se vuelve a instalar
};

SelfPaths SelfPathsFor(const std::string& exe, const std::string& cacheDir);

// La deja el launcher viejo, después de instalar el nuevo y antes de arrancarlo.
bool MarkPending(const SelfPaths& p);

enum class StartupCheck
{
	Normal,       // sin actualización en curso
	Confirming,   // primer arranque después de actualizarse: confirmar con ConfirmHealthy
	RolledBack,   // el nuevo no arrancó: se restauró el viejo; hay que arrancarlo y salir
	RollbackFailed,
};

StartupCheck CheckAtStartup(const SelfPaths& p);

// El nuevo anduvo: borra la marca, el .old y el .bad.
void ConfirmHealthy(const SelfPaths& p);

// Hash de la versión bloqueada (false si no hay ninguna).
bool BlockedVersion(const SelfPaths& p, Update::Digest& out);

} // namespace Launcher
