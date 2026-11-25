#pragma once

// Configuración de MU-Guard (Fase 5): Data/Guard/Guard.toml.  La leen el
// ConnectServer (para anunciarlo al cliente), el GameServer (para exigir el
// ticket en el login) y mu-guard.  Se relee cuando cambia la fecha del archivo,
// así se puede activar o exigir sin reiniciar.
//
//   Enabled = false     # hay un mu-guard corriendo (MU_GUARD_ENABLED)
//   Required = false    # sin ticket válido no se entra (MU_GUARD_REQUIRED)
//   Port = 28980        # puerto público de mu-guard (MU_GUARD_PORT)
//   Socket = ""         # socket local GameServer <-> mu-guard;
//                       # vacío = Data/Guard/mu-guard.sock
//   MemScanCloses = true  # MEMSCAN (un editor escaneando la memoria del
//                         # juego) cierra el juego; false = sólo se registra,
//                         # por si un antivirus da falsos positivos
//   AcceptUnknownBuilds = true  # un main.exe que no está en Data/Guard/Builds
//                               # entra igual, sin desafío de memoria; false =
//                               # sólo los builds publicados
//   ChallengeSeconds = 30   # cada cuánto se desafía la memoria del juego
//                           # (5..300; más corto revisa todo el código antes
//                           # y le cuesta más CPU a mu-guard)
//
// Sin el archivo MU-Guard queda apagado: Data/ es un volumen y las
// instalaciones anteriores no lo tienen (lo crea el entrypoint).

#include <cstdint>
#include <string>

struct GuardSettings
{
	bool Enabled = false;
	bool Required = false;
	std::uint16_t Port = 28980;
	std::string Socket;   // ruta completa
	bool MemScanCloses = true;
	bool AcceptUnknownBuilds = true;
	int ChallengeSeconds = 30;
};

// dataPath: la carpeta Data con la barra final (GetDataPath()).
GuardSettings ReadGuardSettings(const std::string& dataPath);
