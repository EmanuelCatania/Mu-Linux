#pragma once

// "Este server se juega desde el launcher" (ConnectServer.ini [Launcher]
// RequireLauncher, MU_LAUNCHER_REQUIRED en docker).
//
// En la intro, con el canal cifrado ya armado, el cliente le pregunta al
// ConnectServer (F4:16 -> F4:17).  Si el server lo pide y este cliente no lo
// abrió el launcher, abre el launcher y se cierra, como el main.exe del 0.97k
// original con mu.exe.  Si no encuentra el launcher, lo dice y se cierra.
//
// Sólo en escritorio (Windows y Linux): la versión web no tiene launcher y no
// pregunta ni aplica nada.  Un server viejo no contesta F4:16: no pasa nada.
//
// No es seguridad (cualquiera puede poner la variable de entorno del launcher):
// es para que nadie juegue sin actualizar.  La seguridad es la de la Fase 5.

#include <cstdint>

namespace LauncherPolicy {

// Variable de entorno que pone el launcher al abrir el juego.
constexpr const char* kStartedByLauncherEnv = "MU_STARTED_BY_LAUNCHER";

// true en escritorio.
bool Supported();

// Pide la política (F4:16).  Con el canal cifrado; fuera de escritorio no hace nada.
void Request();

// F4:17.
void OnPolicy(const std::uint8_t* msg, int size);

} // namespace LauncherPolicy
