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
// - El latido trae de vuelta los tickets de los jugadores a desconectar (el
//   guardián falló el desafío de memoria o desapareció).  Se aplican en el hilo
//   del juego (QUEUE_TIMER_CLOSE), sólo si la conexión sigue siendo la que entró
//   con ese ticket.
// - Cuando la cuenta entra y cuando elige personaje, se le cuenta a mu-guard
//   quién entró con cada ticket (para los avisos).  Va por el hilo del latido:
//   el del juego no espera.
// - Un login sin ticket también se le consulta, sólo por la IP: una IP baneada
//   no entra aunque no se exija el ticket.
// - A cada jugador se le manda el estado de mu-guard cada 5 s (F1:07, siempre
//   del mismo tamaño, dentro del canal cifrado).  Si se exige el ticket, el que
//   entró con mu-guard caído tiene RevalidateSeconds desde que vuelve para
//   mandar uno nuevo por la misma conexión (F1:08); si no, se lo desconecta.
//
// Sin MU_GUARD (o en Windows) todo esto no existe y el login es el de siempre.

#include <cstdint>

enum class GuardLogin
{
	Accept,        // ticket válido, o MU-Guard apagado
	Unvalidated,   // entra sin validar: no se exige el ticket
	Pending,       // entra sin validar porque mu-guard está caído: valida al volver
	Reject,        // se exige y el ticket no es válido (o no vino)
	Banned         // la IP está baneada por MU-Guard (aunque no se exija el ticket)
};

#if defined(MU_GUARD) && !defined(_WIN32)

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class CGuardLink
{
public:

	// Lanza el hilo del latido.  dataPath: la carpeta Data con la barra final.
	void Start(const std::string& dataPath);

	// ticket/mac: nullptr si el F1:01 no los trae.  transcript: hash del saludo
	// del canal de esta conexión (nullptr si no hay canal).  web: entró por el
	// puerto web (versión del navegador, sin guardián posible).
	GuardLogin CheckLogin(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac,
	                      const std::uint8_t* transcript, bool web);

	// Ata el ticket a la conexión que entró con él.
	void Bind(const std::uint8_t* ticket, int aIndex);

	// La conexión se cerró (gObjDel): su ticket ya no la identifica.
	void Unbind(int aIndex);

	// La cuenta (y el personaje, si ya eligió) de esa conexión, para mu-guard.
	// Hilo del juego; lo manda el hilo del latido.
	void Identify(int aIndex);

	// Desconecta a los jugadores que pidió mu-guard.  Hilo del juego.
	void ProcessKicks();

	// Esa conexión entró con mu-guard caído (GuardLogin::Pending).  Hilo del juego.
	void MarkPending(int aIndex);

	// F1:08: el ticket nuevo de una sesión que entró sin validar.  Hilo del juego.
	void Revalidate(int aIndex, const std::uint8_t* ticket, const std::uint8_t* mac);

	// Cada segundo: el estado a los jugadores (cada 5 s) y el plazo de los que
	// tienen que validar.  Hilo del juego.
	void Tick();

private:

	enum State { Off, Starting, Up, Down };

	void Run();

	int Verify(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac, const std::uint8_t* transcript);

	std::string m_Data;

	std::atomic<int> m_State { Off };

	std::atomic<std::uint32_t> m_Request { 0 };

	std::thread m_Thread;

	// La conexión se reconoce por índice + socket.  ConnectTickCount no sirve:
	// el keep-alive lo reescribe (CGLiveClientRecv).
	struct Session
	{
		int Index;
		std::uint64_t Socket;
	};

	std::mutex m_Lock;   // m_Kicks (latido), m_Sessions y m_Bound

	std::vector<std::string> m_Kicks;

	std::vector<std::vector<std::uint8_t>> m_Identities;   // mensajes Identify pendientes

	std::map<std::string, Session> m_Sessions;   // id del ticket

	std::map<int, std::string> m_Bound;   // índice -> id del ticket

	// Veces que mu-guard volvió (va en el estado: el cliente sabe que es otro aviso).
	std::atomic<std::uint32_t> m_Epoch { 0 };

	// Sesiones que entraron con mu-guard caído.  Sólo el hilo del juego.
	struct PendingSession
	{
		std::uint64_t Socket;
		bool Armed;   // mu-guard volvió y corre el plazo
		std::chrono::steady_clock::time_point Deadline;
	};

	std::map<int, PendingSession> m_Pending;

	std::chrono::steady_clock::time_point m_LastStatus {};
};

extern CGuardLink gGuardLink;

#endif
