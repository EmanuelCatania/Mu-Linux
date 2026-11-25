#pragma once

// Avisos y bans de MU-Guard (paso 5; diseño en doc-port/diseno-seguridad-fase5.md,
// sección 3.2) en la base del server.
//
//   MuGuardWarning  un aviso por guardián que cerró el juego por una detección
//   MuGuardBan      bans de cuenta (además de MEMB_INFO.bloc_code), máquina e IP
//
// mu-guard crea las dos tablas si no están (Data/ y la base son de instalaciones
// anteriores).  Los datos de conexión salen de JoinServer.ini, el mismo que usa
// el JoinServer; DB_HOST, DB_PORT, DB_USER, DB_PASS y DB_NAME lo pisan.
//
// Sin base (caída o mal configurada) mu-guard sigue: la detección cierra igual
// el juego y el aviso queda sólo en el log.

#include "GuardConfig.h"

#include <cstdint>
#include <map>
#include <string>

struct GuardWarning
{
	int code = 0;               // GuardProto::Detection
	std::string detail;
	std::string account;        // vacío si se detectó antes del login
	std::string character;
	std::string machine;        // hash con la sal del server, en hex; vacío si no hay
	std::string ip;
	std::string build;          // primeros bytes del hash del ejecutable, en hex
};

struct GuardWarningResult
{
	bool stored = false;        // quedó en la base
	int number = 0;             // aviso número (el mayor entre cuenta y máquina)
	std::uint8_t banned = 0;    // GuardProto::BanFlag
	std::int64_t until = 0;     // segundos Unix; 0 = permanente
};

class CGuardStore
{
public:

	~CGuardStore();

	// Conecta si hace falta (con reintentos espaciados).  false: sin base.
	bool Ready();

	GuardWarningResult AddWarning(const GuardWarning& warning, const GuardSettings& settings);

	// Ban vigente de una máquina o una IP (de la caché, se relee cada 30 s).
	// until: segundos Unix, 0 = permanente.
	bool IsBanned(const std::string& kind, const std::string& value, std::int64_t& until);

	// Relee la caché de bans si toca.
	void Tick();

private:

	bool Connect();

	bool Exec(const std::string& query);

	// Primera columna de la primera fila como entero (o `fallback`).
	std::int64_t Scalar(const std::string& query, std::int64_t fallback);

	std::string Quote(const std::string& value);

	void LoadBans();

	// Inserta el ban y lo suma a la caché.  amount en `unit` (DAY/HOUR); 0 = permanente.
	std::int64_t AddBan(const std::string& kind, const std::string& value, const std::string& reason, int amount, const char* unit);

	void* m_Mysql = nullptr;   // MYSQL*

	std::int64_t m_NextAttempt = 0;

	std::int64_t m_NextBanLoad = 0;

	std::map<std::string, std::int64_t> m_Bans;   // "tipo:valor" -> hasta
};
