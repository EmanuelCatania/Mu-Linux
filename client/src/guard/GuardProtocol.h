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

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GuardProto {

constexpr std::uint16_t Version = 1;

constexpr std::uint16_t DefaultPort = 28980;   // fuera de los puertos del server y del rango efímero

constexpr std::size_t TicketIdSize = 16;
constexpr std::size_t TicketKeySize = 32;
constexpr std::size_t MacSize = 32;
constexpr std::size_t TranscriptSize = 32;
constexpr std::size_t HashSize = 32;
constexpr std::size_t IpSize = 46;   // texto, terminado en cero (entra una IPv6)

constexpr unsigned TicketSeconds = 60;

constexpr char TicketContext[] = "MuTicket1";

// Tope de un mensaje (tipo + datos).
constexpr std::size_t MaxMessage = 4096;

enum Type : std::uint8_t
{
	// Guardián -> mu-guard
	Hello = 0x01,           // versión (2, LE) | hash del main en disco (32)
	TicketRequest = 0x02,   // sin datos

	// mu-guard -> guardián
	HelloResult = 0x81,     // resultado (1)
	Ticket = 0x82,          // resultado (1) | id (16) | clave (32)

	// GameServer -> mu-guard (socket local)
	Ping = 0x10,            // sin datos
	Verify = 0x11,          // pedido (4, LE) | id (16) | MAC (32) | hash del saludo (32) | IP (46)

	// mu-guard -> GameServer
	Pong = 0x90,            // sin datos
	VerifyResult = 0x91,    // pedido (4, LE) | resultado (1)
};

// Resultado de Hello y Ticket.
enum Result : std::uint8_t
{
	Ok = 0,
	BadVersion = 1,   // el guardián habla otra versión de este protocolo
	NotReady = 2,     // falta el saludo, o mu-guard no pudo armar el ticket
};

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
};

// Largos de los datos de cada mensaje.
constexpr std::size_t HelloSize = 2 + HashSize;
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

} // namespace GuardProto
