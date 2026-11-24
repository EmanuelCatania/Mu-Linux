#pragma once

// Enlace del GameServer con mu-guard (MU-Guard, Fase 5; diseño en
// doc-port/diseno-seguridad-fase5.md, secciones 2 y 4c).
//
// - Un hilo propio le manda un latido por el socket local cada segundo.  Con 5
//   perdidos (o el socket cerrado) mu-guard pasa a CAIDO; con 3 seguidos vuelve
//   a ARRIBA.  Al arrancar espera 10 s antes de declararlo caído.
// - En el login (CGConnectAccountRecv) se le consulta el ticket que trae el
//   F1:01.  Con mu-guard CAIDO no se le pregunta nada: se deja entrar y se
//   anota la sesión como "sin validar".
//
// Sin MU_GUARD (o en Windows) todo esto no existe y el login es el de siempre.

#include <cstdint>

enum class GuardLogin
{
	Accept,        // ticket válido, o MU-Guard apagado
	Unvalidated,   // entra sin validar: mu-guard caído, o no se exige el ticket
	Reject         // se exige y el ticket no es válido (o no vino)
};

#if defined(MU_GUARD) && !defined(_WIN32)

#include <atomic>
#include <string>
#include <thread>

class CGuardLink
{
public:

	// Lanza el hilo del latido.  dataPath: la carpeta Data con la barra final.
	void Start(const std::string& dataPath);

	// ticket/mac: nullptr si el F1:01 no los trae.  transcript: hash del saludo
	// del canal de esta conexión (nullptr si no hay canal).
	GuardLogin CheckLogin(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac,
	                      const std::uint8_t* transcript);

private:

	enum State { Off, Starting, Up, Down };

	void Run();

	int Verify(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac, const std::uint8_t* transcript);

	std::string m_Data;

	std::atomic<int> m_State { Off };

	std::atomic<std::uint32_t> m_Request { 0 };

	std::thread m_Thread;
};

extern CGuardLink gGuardLink;

#endif
