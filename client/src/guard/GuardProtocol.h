#pragma once

// MU-Guard (Fase 5): mensajes entre el guardián del cliente y mu-guard, y entre
// mu-guard y el GameServer.  Diseño en doc-port/diseno-seguridad-fase5.md.
//
// ESTE ARCHIVO ES EL MISMO EN EL CLIENTE Y EN EL SERVER
// (client/src/guard/GuardProtocol.h y server/Source/MuServer/Common/GuardProtocol.h).
// Un cambio en uno va también al otro, y sube Version.
//
// Guardián <-> mu-guard: conexión TCP al puerto de mu-guard, con el mismo canal
// cifrado (SecureChannel) y la misma identidad del server que el juego.
// mu-guard <-> GameServer: socket local (Unix), sin cifrar: no sale de la máquina.
//
// En los dos, cada mensaje es:
//   largo (2 bytes, little-endian: tipo + datos) | tipo (1) | datos
//
// El ticket:
//   1. el juego se lo pide al guardián por el pipe heredado, en cada intento de
//      login;
//   2. el guardián se lo pide a mu-guard (TicketRequest), que lo ata a la IP de
//      la conexión del guardián: id (16 bytes) + clave (32), un solo uso,
//      TicketSeconds de vida;
//   3. el juego manda al final del F1:01 el id y
//      TicketMac = BLAKE2b-256(clave = clave del ticket,
//                              mensaje = TicketContext + hash del saludo)
//      con el hash del saludo de SU conexión con el GameServer
//      (SecureChannel::TranscriptHash);
//   4. el GameServer le pasa a mu-guard el id, el MAC, el hash del saludo de esa
//      conexión y la IP (Verify).  Un MAC calculado en otra conexión no verifica.
//
// Avisos y bans (paso 5): el guardián manda en el saludo el hash de su máquina
// (MachineContext); mu-guard lo guarda con una sal propia del server.  El
// GameServer le cuenta qué cuenta y qué personaje entraron con cada ticket
// (Identify).  Una detección que cierra el juego suma un aviso (uno por
// guardián); a los X avisos se banea cuenta, máquina e IP.  Notice le dice al
// guardián qué aviso fue, para el banner.
//
// Listas y textos del server (paso 5c, Data/Guard/Lists.toml y Messages.toml):
// después del saludo mu-guard manda Rules (un RuleReset y una entrada por
// mensaje), y de nuevo cuando cambian los archivos.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GuardProto {

constexpr std::uint16_t Version = 4;   // 2: Report; 3: desafío de memoria; 4: avisos y bans

constexpr std::uint16_t DefaultPort = 28980;   // fuera de los puertos del server y del rango efímero

constexpr std::size_t TicketIdSize = 16;
constexpr std::size_t TicketKeySize = 32;
constexpr std::size_t MacSize = 32;
constexpr std::size_t TranscriptSize = 32;
constexpr std::size_t HashSize = 32;
constexpr std::size_t IpSize = 46;   // texto, terminado en cero (entra una IPv6)
constexpr std::size_t GuardianIdSize = 16;   // al azar, uno por guardián (sobrevive a reconectar)
constexpr std::size_t NonceSize = 16;
constexpr std::size_t MachineSize = 32;   // hash de la máquina (MachineContext)
constexpr std::size_t AccountSize = 10;   // cuenta y personaje, sin el cero final

constexpr unsigned TicketSeconds = 60;

// Desafío de memoria (diseño, punto 6 del flujo): cada ChallengeSeconds (con
// algo de azar) mu-guard pide el hash de rangos del código del juego.  Sin
// respuesta en ChallengeTimeoutSeconds, o con el guardián desconectado más de
// GuardianGraceSeconds, se desconecta al jugador.
constexpr unsigned ChallengeSeconds = 30;
constexpr unsigned ChallengeTimeoutSeconds = 20;
constexpr unsigned GuardianGraceSeconds = 30;
constexpr std::size_t MaxChallengeRanges = 8;
constexpr std::uint32_t MaxChallengeRange = 64 * 1024;

constexpr char TicketContext[] = "MuTicket1";

// Hash de la máquina: BLAKE2b-256(MachineContext + lo que la identifica).
constexpr char MachineContext[] = "MuMachine1";

// Tope de un mensaje (tipo + datos).
constexpr std::size_t MaxMessage = 4096;

enum Type : std::uint8_t
{
	// Guardián -> mu-guard
	Hello = 0x01,           // versión (2, LE) | hash del main en disco (32) | id del guardián (16) | máquina (32)
	TicketRequest = 0x02,   // sin datos
	Report = 0x03,          // detección (1) | cerró el juego (1) | detalle (texto UTF-8, hasta MaxDetail)
	ChallengeAnswer = 0x04, // desafío (4, LE) | base del ejecutable en el juego (8, LE) | hash (32)

	// mu-guard -> guardián
	HelloResult = 0x81,     // resultado (1) | opciones (1, HelloOption) [| Banned: qué (1, BanFlag) | hasta (8, LE)]
	Ticket = 0x82,          // resultado (1) | id (16) | clave (32)
	Challenge = 0x83,       // desafío (4, LE) | nonce (16) | rangos (1) | rangos x (rva (4, LE) | largo (4, LE))
	Verdict = 0x84,         // detección (1) | detalle (texto): cerrar el juego por orden del server
	Notice = 0x85,          // aviso (1) | límite (1, 0 = no se banea) | baneado (1, BanFlag) | hasta (8, LE):
	                        // respuesta a una detección que cierra el juego
	Rules = 0x86,           // tipo (1, RuleKind) | texto (UTF-8, hasta MaxRule)

	// GameServer -> mu-guard (socket local)
	Ping = 0x10,            // sin datos
	Verify = 0x11,          // pedido (4, LE) | id (16) | MAC (32) | hash del saludo (32) | IP (46)
	                        // (id en ceros: login sin ticket; sólo se mira si la IP está baneada)
	Identify = 0x12,        // id (16) | cuenta (10) | personaje (10): quién entró con ese ticket

	// mu-guard -> GameServer
	Pong = 0x90,            // jugadores a desconectar (2, LE) | ids de sus tickets (16 cada uno)
	VerifyResult = 0x91,    // pedido (4, LE) | resultado (1)
};

// Resultado de Hello y Ticket.
enum Result : std::uint8_t
{
	Ok = 0,
	BadVersion = 1,   // el guardián habla otra versión de este protocolo
	NotReady = 2,     // falta el saludo, o mu-guard no pudo armar el ticket
	UnknownBuild = 3, // el ejecutable no está entre los builds aceptados (Data/Guard/Builds)
	Banned = 4,       // la máquina o la IP están baneadas
};

// Qué quedó baneado (HelloResult y Notice).  "Hasta" en segundos Unix; 0 =
// permanente.
enum BanFlag : std::uint8_t
{
	BanAccount = 0x01,
	BanMachine = 0x02,
	BanIp = 0x04,
};

// Opciones del server para el guardián (Data/Guard/Guard.toml), en HelloResult.
enum HelloOption : std::uint8_t
{
	MemScanCloses = 0x01,   // MEMSCAN cierra el juego (si no, sólo se registra)
	ToolCloses = 0x02,      // TOOL (una herramienta de las listas abierta) cierra el juego
};

// Entradas de Rules.
enum RuleKind : std::uint8_t
{
	RuleReset = 0,          // sin texto: empieza una lista nueva (borra la anterior)
	RuleProcess = 1,        // nombre de un ejecutable (sin mayúsculas): TOOL
	RuleWindow = 2,         // parte del título de una ventana (sin mayúsculas): TOOL
	RuleModuleBlock = 3,    // parte del nombre de un módulo cargado en el juego: INJECT que cierra
	RuleModuleAllow = 4,    // parte de la ruta de un módulo: no se reporta
	RuleName = 5,           // idioma (es, en, pt) TAB detección (DEBUGGER...) TAB nombre para el banner
};

constexpr std::size_t MaxRule = 255;
constexpr std::size_t MaxRules = 2000;

// Resultado de Verify.
enum VerifyCode : std::uint8_t
{
	Valid = 0,
	UnknownTicket = 1,
	Expired = 2,
	AlreadyUsed = 3,
	WrongIp = 4,
	WrongMac = 5,
	Malformed = 6,
	BannedIp = 7,     // la IP está baneada (también sin ticket)
};

// Lo que detectó el guardián (diseño, sección 3).  Los que cierran el juego
// dejan un aviso en el server (paso 5); el resto sólo se registra.
enum Detection : std::uint8_t
{
	Debugger = 1,   // depurador sobre el juego o el guardián
	Inject = 2,     // módulo cargado en el juego que no es del juego ni del sistema
	MemEdit = 3,    // editor de memoria enganchado al juego (su driver, o /proc/<pid>/mem)
	MemScan = 4,    // alguien leyó las páginas señuelo del juego
	Patch = 5,      // código del juego modificado en memoria (desafío, paso 4)
	Files = 6,      // archivos del cliente que no coinciden
	Tool = 7,       // proceso o ventana de las listas
};

constexpr std::size_t MaxDetail = 512;

// Largos de los datos de cada mensaje.
constexpr std::size_t HelloSize = 2 + HashSize + GuardianIdSize + MachineSize;
constexpr std::size_t HelloResultBannedSize = 2 + 1 + 8;
constexpr std::size_t NoticeSize = 1 + 1 + 1 + 8;
constexpr std::size_t IdentifySize = TicketIdSize + AccountSize + AccountSize;
constexpr std::size_t ChallengeAnswerSize = 4 + 8 + HashSize;
constexpr std::size_t TicketSize = 1 + TicketIdSize + TicketKeySize;
constexpr std::size_t VerifySize = 4 + TicketIdSize + MacSize + TranscriptSize + IpSize;
constexpr std::size_t VerifyResultSize = 4 + 1;

// Agrega un mensaje a `out`.
inline void Append(std::vector<std::uint8_t>& out, std::uint8_t type, const void* data, std::size_t size)
{
	const std::size_t length = size + 1;
	out.push_back(static_cast<std::uint8_t>(length & 0xFF));
	out.push_back(static_cast<std::uint8_t>(length >> 8));
	out.push_back(type);
	const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
	if (size != 0) out.insert(out.end(), p, p + size);
}

// Saca el primer mensaje completo de `in`.  1: hay uno (tipo y datos en
// type/payload), 0: falta llegar más, -1: mal formado (cerrar la conexión).
inline int Pop(std::vector<std::uint8_t>& in, std::uint8_t& type, std::vector<std::uint8_t>& payload)
{
	if (in.size() < 2) return 0;
	const std::size_t length = static_cast<std::size_t>(in[0]) | (static_cast<std::size_t>(in[1]) << 8);
	if (length == 0 || length > MaxMessage) return -1;
	if (in.size() < 2 + length) return 0;
	type = in[2];
	payload.assign(in.begin() + 3, in.begin() + 2 + static_cast<std::ptrdiff_t>(length));
	in.erase(in.begin(), in.begin() + 2 + static_cast<std::ptrdiff_t>(length));
	return 1;
}

inline void PutLe32(std::uint8_t* p, std::uint32_t v)
{
	p[0] = static_cast<std::uint8_t>(v);
	p[1] = static_cast<std::uint8_t>(v >> 8);
	p[2] = static_cast<std::uint8_t>(v >> 16);
	p[3] = static_cast<std::uint8_t>(v >> 24);
}

inline std::uint32_t GetLe32(const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
	       static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

inline void PutLe64(std::uint8_t* p, std::uint64_t v)
{
	PutLe32(p, static_cast<std::uint32_t>(v));
	PutLe32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

inline std::uint64_t GetLe64(const std::uint8_t* p)
{
	return static_cast<std::uint64_t>(GetLe32(p)) | static_cast<std::uint64_t>(GetLe32(p + 4)) << 32;
}

// Contexto del hash del desafío: BLAKE2b-256(clave = nonce, mensaje = los
// bytes de los rangos, en orden).
constexpr char ChallengeContext[] = "MuChallenge1";

} // namespace GuardProto
