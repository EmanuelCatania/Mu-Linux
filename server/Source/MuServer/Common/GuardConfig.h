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
//   [warnings]              # avisos (sólo los lee mu-guard)
//   limit = 3               # avisos para banear; 0 = nunca banear
//   window_days = 30        # los avisos más viejos no cuentan
//   codes = ["DEBUGGER", "INJECT", "MEMEDIT", "PATCH"]   # MEMSCAN no suma por defecto
//
//   [ban]
//   account_days = 7        # 0 = permanente, -1 = no banear la cuenta
//   machine_days = 7        # igual, para la máquina
//   ip_hours = 24           # igual, en horas; nunca IP privadas ni loopback
//   exempt_accounts = []    # se registran pero no se banean (staff)
//   exempt_ips = []
//
//   ToolCloses = true       # una herramienta de Lists.toml abierta cierra el
//                           # juego (false: sólo se registra)
//   MaxPerMachine = 0       # juegos abiertos a la vez en una misma PC (0 = sin
//                           # límite); el que se pasa no se abre
//   FilesCloses = true      # un archivo distinto de Files.txt cierra el juego
//                           # (false: sólo se registra)
//
// Lists.toml, Messages.toml (paso 5c) y Files.txt (FILES) los lee sólo mu-guard
// (ReadGuardRules).
//
// Sin el archivo MU-Guard queda apagado: Data/ es un volumen y las
// instalaciones anteriores no lo tienen (lo crea el entrypoint).

#include <cstdint>
#include <string>
#include <vector>

struct GuardSettings
{
	bool Enabled = false;
	bool Required = false;
	std::uint16_t Port = 28980;
	std::string Socket;   // ruta completa
	bool MemScanCloses = true;
	bool AcceptUnknownBuilds = true;
	bool ToolCloses = true;
	int MaxPerMachine = 0;   // 0 = sin límite
	bool FilesCloses = true;
	int ChallengeSeconds = 30;

	// [warnings]
	int WarningLimit = 3;
	int WarningWindowDays = 30;
	std::uint32_t WarningCodes = 0;   // bit (1 << GuardProto::Detection); se llena al leer

	// [ban]
	int BanAccountDays = 7;
	int BanMachineDays = 7;
	int BanIpHours = 24;
	std::vector<std::string> ExemptAccounts;   // en minúsculas
	std::vector<std::string> ExemptIps;
};

// Nombre de una detección (GuardProto::Detection), o "?".
const char* GuardDetectionName(int code);

// Listas y textos para el guardián (Data/Guard/Lists.toml y Messages.toml).
// Todo en minúsculas salvo los nombres.  version cambia cuando cambia alguno de
// los dos archivos.
struct GuardRules
{
	std::vector<std::string> Processes;
	std::vector<std::string> Windows;
	std::vector<std::string> ModuleBlock;
	std::vector<std::string> ModuleAllow;
	std::vector<std::string> Names;   // "idioma\tDETECCION\tnombre"
	std::vector<std::string> Files;   // "hash en hex\truta" (Files.txt)
	std::uint64_t Version = 0;
};

GuardRules ReadGuardRules(const std::string& dataPath);

// dataPath: la carpeta Data con la barra final (GetDataPath()).
GuardSettings ReadGuardSettings(const std::string& dataPath);
