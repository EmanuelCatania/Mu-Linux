#pragma once

// "Hay un cliente corriendo en esta instalación" (diseño del launcher, 4b).
//
// El cliente toma un lock compartido sobre User/Cache/running.lock al arrancar y
// lo tiene hasta que termina (el sistema lo suelta solo si el proceso se cae).
// Varias instancias del juego lo comparten.  El launcher lo consulta para saber
// si el juego está abierto: con el juego abierto no reemplaza archivos (en
// Windows uno abierto no se puede pisar) ni escribe Config.ini.
//
// Lo comparten el cliente y el launcher: sin PCH ni nada de la plataforma.

#include <string>

namespace RunningLock {

// Cliente: toma el lock y lo deja tomado.  false si no se pudo (no es grave: el
// launcher no va a saber que el juego está abierto).
bool Acquire(const std::string& path);

// Cliente: lo suelta antes de terminar (al abrir el launcher, ver
// net/LauncherPolicy.h).  Sin Acquire no hace nada.
void Release();

// Launcher: true si algún cliente lo tiene tomado.
bool IsHeld(const std::string& path);

} // namespace RunningLock
