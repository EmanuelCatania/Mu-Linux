#pragma once

// launcher.json: la configuración que el server le da al launcher (diseño, 6).
//
// La arma y la firma `mu-patch` con la identidad del server (contexto
// MuLauncher1) junto al manifiesto de sus parches; la reparte el ConnectServer
// (F4:14/15, por el canal cifrado) y mu-web en /updates/.  El launcher la guarda
// en su caché y la usa en el próximo arranque: así el operador cambia la
// dirección del ConnectServer, la de los parches o el origen de la base sin
// redistribuir el launcher.
//
//   {
//     "format": 1,
//     "sequence": 3,
//     "connectServers": [ "mu.ejemplo.com:44405", "respaldo.ejemplo.com:44405" ],
//     "patches": { "channel": "miserver/overlay", "urls": [ "https://mu.ejemplo.com/updates" ] },
//     "base": { "source": "dueño/repo", "tag": "0.97.50", "key": "<64 hex>" },
//     "news": "Texto que se muestra en el launcher",
//     "web": "https://mu.ejemplo.com"
//   }
//
// Todo menos format y sequence es opcional.  La secuencia sube cada vez que
// cambia algo y el launcher no acepta una menor que la que ya tiene: sin eso se
// podría reenviar una configuración vieja con firma válida.

#include <cstdint>
#include <string>
#include <vector>

namespace Update {

constexpr int kAnnounceFormat = 1;
constexpr std::size_t kAnnounceMaxServers = 8;
constexpr std::size_t kAnnounceMaxUrls = 8;
constexpr std::size_t kAnnounceMaxNews = 2000;
// Tope del archivo entero: lo que el ConnectServer acepta reenviar.
constexpr std::size_t kAnnounceMaxSize = 16 * 1024;

struct Announce
{
	std::uint64_t sequence = 0;
	std::vector<std::string> connectServers;   // "host:puerto"
	std::string patchChannel;
	std::vector<std::string> patchUrls;
	std::string baseSource;                    // "dueño/repo" o una URL
	std::string baseTag;
	std::string baseKey;                       // 64 hex
	std::string news;
	std::string web;                           // sitio del server (botón en Opciones > General)
};

std::string DumpAnnounce(const Announce& a);

// Un valor fuera de formato o un archivo más grande que kAnnounceMaxSize lo
// rechazan.  Un campo desconocido se ignora: así un server puede agregar uno sin
// dejar afuera a los launchers anteriores (un cambio incompatible sube format).
bool ParseAnnounce(const std::string& text, Announce& out, std::string* error);

// Igual salvo la secuencia.
bool SameAnnounceContent(const Announce& a, const Announce& b);

// "host:puerto": nombre o IPv4, puerto 1..65535.
bool ParseHostPort(const std::string& text, std::string* host, std::uint16_t* port);

} // namespace Update
