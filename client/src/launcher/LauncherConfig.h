#pragma once

// Configuración del launcher: la semilla compilada por el operador y, encima,
// User/launcher.toml (lo que puede cambiar el jugador, y para pruebas el
// origen de la base).
//
// Compilada (opciones de CMake, ver src/launcher/CMakeLists.txt):
//   MU_LAUNCHER_TITLE     título de la ventana
//   MU_BASE_REPO          repo de GitHub con los Releases de la base
//   MU_BASE_TAG           tag de la base (por defecto MU_RELEASE del cliente)
//   MU_BASE_KEY           clave pública que firma esos Releases (64 hex)
//   MU_OVERLAY_URL        carpeta HTTP con los parches del operador
//   MU_OVERLAY_CHANNEL    su canal ("miserver/overlay")
//   MU_OVERLAY_KEY        identidad del server (64 hex)
//   MU_CONNECT_SERVER     ConnectServer del server ("host:puerto", varios con coma):
//                         de ahí se pide launcher.json (ServerAnnounce.h)
//   MU_WEB_URL            sitio del server, si no lo anuncia launcher.json
//   MU_GAME_EXE           ejecutable del cliente
//
// launcher.toml (todo opcional):
//   Music = true          bajar la música
//   HD = false            bajar las texturas HD
//   Connections = 4
//   SpeedLimitKB = 0      0: sin límite
//   PlayingSpeedLimitKB = 512   con el juego abierto (una conexión); 0: sin límite
//   Scale = 0             tamaño de la ventana en %, 100..200; 0: el de la pantalla
//   BaseSource = "dueño/repo" o una URL   (pruebas: se avisa en la ventana)
//   BaseTag = "0.97.50"                   (pruebas)
//   BaseKey = "<64 hex>"                  (pruebas)

#include <cstdint>
#include <string>
#include <vector>

namespace Launcher {

struct Config
{
	std::string title;
	std::string baseRepo;          // "dueño/repo"; vacío si baseUrl
	std::string baseUrl;           // carpeta con los assets de un Release (en vez de GitHub)
	std::string baseTag;
	std::string baseKey;           // hex; vacío: la base no se baja
	std::string overlayUrl;
	std::string overlayChannel;
	std::string overlayKey;
	std::vector<std::string> connectServers;   // "host:puerto"
	std::string webUrl;
	std::string gameExe;
	bool music = true;
	bool hd = false;
	bool hdSet = false;            // launcher.toml fija HD (si no, sigue a HDTextures del juego)
	int connections = 4;
	std::uint64_t speedLimit = 0;  // bytes/s
	std::uint64_t playingSpeedLimit = 512 * 1024;   // bytes/s con el juego abierto; 0: sin límite
	int scale = 0;                 // % del tamaño de la ventana; 0: el de la pantalla
	bool fullVerify = false;       // "Verificar archivos": rehashear todo
	// El launcher que corre, relativo a la raíz ("Launcher/launcher.exe"); vacío
	// si está fuera de la carpeta del cliente (no se actualiza a sí mismo).
	std::string selfPath;
	// Lo que launcher.toml cambió del origen de la base: se muestra como aviso.
	std::vector<std::string> overrides;
};

Config CompiledConfig();

// Aplica launcher.toml.  Las líneas que no entiende las ignora y las anota en
// *warnings.  Un archivo que falta no es error.
void ApplyLauncherToml(const std::string& path, Config& config, std::vector<std::string>* warnings);

// [Window] HDTextures del Config.ini del juego (0 si falta o no se puede leer).
bool HDTexturesEnabled(const std::string& configIni);

// [Language] LangSelection del juego (vacío si no está): el idioma del launcher.
std::string LanguageSetting(const std::string& configIni);

// La carpeta de usuario del cliente para una instalación: <raíz>/User en
// Windows; la raíz misma en Linux, donde el launcher arranca el cliente con
// MU_CLIENT_ROOT (ver platform/UserPath.h).
std::string UserDirFor(const std::string& root);

} // namespace Launcher
