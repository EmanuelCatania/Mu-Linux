#pragma once

// Configuración de MU-Guard (Fase 5): Data/Guard/Guard.toml.  La leen el
// ConnectServer (para anunciarlo al cliente), el GameServer (para exigir el
// ticket en el login) y mu-guard.  Se relee cuando cambia la fecha del archivo,
// así se puede activar o exigir sin reiniciar.
//
//   Enabled = false     # hay un mu-guard corriendo (MU_GUARD_ENABLED)
//   Required = false    # sin ticket válido no se entra (MU_GUARD_REQUIRED)
//   Port = 55980        # puerto público de mu-guard
//   Socket = ""         # socket local GameServer <-> mu-guard;
//                       # vacío = Data/Guard/mu-guard.sock
//
// Sin el archivo MU-Guard queda apagado: Data/ es un volumen y las
// instalaciones anteriores no lo tienen (lo crea el entrypoint).

#include <cstdint>
#include <string>

struct GuardSettings
{
	bool Enabled = false;
	bool Required = false;
	std::uint16_t Port = 55980;
	std::string Socket;   // ruta completa
};

// dataPath: la carpeta Data con la barra final (GetDataPath()).
GuardSettings ReadGuardSettings(const std::string& dataPath);
