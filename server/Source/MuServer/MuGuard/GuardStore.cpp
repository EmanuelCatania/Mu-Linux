#include "stdafx.h"
#include "GuardStore.h"
#include "GuardProtocol.h"

// mu_link_mysql define MYSQL (lo usan JoinServer y DataServer para elegir
// la rama de QueryManager); acá choca con el tipo MYSQL de mysql.h.
#undef MYSQL
#include <mysql.h>

#include <arpa/inet.h>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <unistd.h>

namespace
{
	constexpr int kRetrySeconds = 30;
	constexpr int kBanReloadSeconds = 30;

	MYSQL* Handle(void* p)
	{
		return static_cast<MYSQL*>(p);
	}

	std::int64_t Now()
	{
		return (std::int64_t)std::time(nullptr);
	}

	std::string Trim(const std::string& text)
	{
		const std::size_t first = text.find_first_not_of(" \t\r\n");

		if (first == std::string::npos)
		{
			return std::string();
		}

		return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
	}

	// JoinServer.ini: junto al ejecutable de mu-guard (/opt/mu/mu-guard ->
	// /opt/mu/JoinServer/JoinServer.ini), o MU_GUARD_DB_INI.
	std::map<std::string, std::string> ReadIni()
	{
		std::string path;

		const char* env = std::getenv("MU_GUARD_DB_INI");

		if (env != nullptr && env[0] != '\0')
		{
			path = env;
		}
		else
		{
			char exe[4096];

			const ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);

			std::string dir = n > 0 ? std::string(exe, (std::size_t)n) : std::string();

			dir = dir.substr(0, dir.find_last_of('/') + 1);

			path = (dir.empty() ? std::string("./") : dir) + "JoinServer/JoinServer.ini";
		}

		std::map<std::string, std::string> values;

		std::ifstream file(path);

		std::string line;

		while (std::getline(file, line))
		{
			const std::size_t equal = line.find('=');

			if (equal != std::string::npos && line[0] != ';' && line[0] != '[')
			{
				values[Trim(line.substr(0, equal))] = Trim(line.substr(equal + 1));
			}
		}

		return values;
	}

	std::string Setting(const std::map<std::string, std::string>& ini, const char* key, const char* env)
	{
		const char* value = std::getenv(env);

		if (value != nullptr && value[0] != '\0')
		{
			return value;
		}

		auto it = ini.find(key);

		return it == ini.end() ? std::string() : it->second;
	}

	// IP que no se banea nunca: privadas, loopback, enlace local y CGNAT (todos
	// los que llegan detrás de un proxy o de algunos Docker comparten la misma).
	bool PublicIp(const std::string& ip)
	{
		in_addr address {};

		if (::inet_pton(AF_INET, ip.c_str(), &address) != 1)
		{
			return false;
		}

		const std::uint32_t a = ntohl(address.s_addr);

		const auto in = [a](std::uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };

		return !(in(0x0A000000, 8) || in(0x7F000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) ||
			in(0xA9FE0000, 16) || in(0x64400000, 10) || in(0x00000000, 8));
	}

	bool Contains(const std::vector<std::string>& list, const std::string& value)
	{
		for (const std::string& item : list)
		{
			if (item == value)
			{
				return true;
			}
		}

		return false;
	}
}

CGuardStore::~CGuardStore()
{
	if (this->m_Mysql != nullptr)
	{
		mysql_close(Handle(this->m_Mysql));
	}
}

bool CGuardStore::Connect()
{
	const std::map<std::string, std::string> ini = ReadIni();

	std::string host = Setting(ini, "DataBaseHost", "DB_HOST");

	if (host.compare(0, 6, "tcp://") == 0)
	{
		host = host.substr(6);
	}

	const std::string port = Setting(ini, "DataBasePort", "DB_PORT");
	const std::string user = Setting(ini, "DataBaseUser", "DB_USER");
	const std::string pass = Setting(ini, "DataBasePass", "DB_PASS");
	const std::string name = Setting(ini, "DataBaseName", "DB_NAME");

	if (host.empty() || user.empty() || name.empty())
	{
		LogAdd(LOG_RED, "[MU-Guard] Sin datos de la base (JoinServer.ini o DB_*): los avisos quedan solo en el log");

		return false;
	}

	MYSQL* mysql = mysql_init(nullptr);

	if (mysql == nullptr)
	{
		return false;
	}

	unsigned int timeout = 3;

	mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

	timeout = 5;

	mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, &timeout);

	mysql_options(mysql, MYSQL_OPT_WRITE_TIMEOUT, &timeout);

	mysql_options(mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

	if (mysql_real_connect(mysql, host.c_str(), user.c_str(), pass.c_str(), name.c_str(), (unsigned)std::atoi(port.c_str()), nullptr, 0) == nullptr)
	{
		LogAdd(LOG_RED, "[MU-Guard] No se pudo conectar a la base %s@%s: %s", name.c_str(), host.c_str(), mysql_error(mysql));

		mysql_close(mysql);

		return false;
	}

	this->m_Mysql = mysql;

	const bool tables = this->Exec(
		"CREATE TABLE IF NOT EXISTS `MuGuardWarning` ("
		" `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
		" `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		" `code` VARCHAR(16) NOT NULL,"
		" `detail` VARCHAR(512) NOT NULL DEFAULT '',"
		" `account` VARCHAR(10) NOT NULL DEFAULT '',"
		" `character_name` VARCHAR(10) NOT NULL DEFAULT '',"
		" `machine` CHAR(32) NOT NULL DEFAULT '',"
		" `ip` VARCHAR(46) NOT NULL DEFAULT '',"
		" `build` VARCHAR(16) NOT NULL DEFAULT '',"
		" `number` INT NOT NULL DEFAULT 0,"
		" `voided` TINYINT(1) NOT NULL DEFAULT 0,"
		" PRIMARY KEY (`id`),"
		" KEY `ix_account` (`account`, `created_at`),"
		" KEY `ix_machine` (`machine`, `created_at`),"
		" KEY `ix_created` (`created_at`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4") &&
		this->Exec(
		"CREATE TABLE IF NOT EXISTS `MuGuardBan` ("
		" `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,"
		" `created_at` DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,"
		" `kind` VARCHAR(8) NOT NULL,"
		" `value` VARCHAR(64) NOT NULL,"
		" `reason` VARCHAR(128) NOT NULL DEFAULT '',"
		" `expires_at` DATETIME NULL,"
		" `lifted` TINYINT(1) NOT NULL DEFAULT 0,"
		" PRIMARY KEY (`id`),"
		" KEY `ix_subject` (`kind`, `value`)"
		") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

	if (!tables)
	{
		mysql_close(mysql);

		this->m_Mysql = nullptr;

		return false;
	}

	LogAdd(LOG_BLUE, "[MU-Guard] Base conectada (%s@%s): avisos y bans activos", name.c_str(), host.c_str());

	this->m_NextBanLoad = 0;

	return true;
}

bool CGuardStore::Ready()
{
	if (this->m_Mysql != nullptr)
	{
		if (mysql_ping(Handle(this->m_Mysql)) == 0)
		{
			return true;
		}

		LogAdd(LOG_RED, "[MU-Guard] Se perdio la base: %s", mysql_error(Handle(this->m_Mysql)));

		mysql_close(Handle(this->m_Mysql));

		this->m_Mysql = nullptr;
	}

	if (Now() < this->m_NextAttempt)
	{
		return false;
	}

	this->m_NextAttempt = Now() + kRetrySeconds;

	return this->Connect();
}

bool CGuardStore::Exec(const std::string& query)
{
	if (mysql_query(Handle(this->m_Mysql), query.c_str()) != 0)
	{
		LogAdd(LOG_RED, "[MU-Guard] Error de la base: %s", mysql_error(Handle(this->m_Mysql)));

		return false;
	}

	// Una consulta que devuelve filas: se descartan.
	if (MYSQL_RES* result = mysql_store_result(Handle(this->m_Mysql)))
	{
		mysql_free_result(result);
	}

	return true;
}

std::int64_t CGuardStore::Scalar(const std::string& query, std::int64_t fallback)
{
	if (mysql_query(Handle(this->m_Mysql), query.c_str()) != 0)
	{
		LogAdd(LOG_RED, "[MU-Guard] Error de la base: %s", mysql_error(Handle(this->m_Mysql)));

		return fallback;
	}

	MYSQL_RES* result = mysql_store_result(Handle(this->m_Mysql));

	if (result == nullptr)
	{
		return fallback;
	}

	std::int64_t value = fallback;

	MYSQL_ROW row = mysql_fetch_row(result);

	if (row != nullptr && row[0] != nullptr)
	{
		value = std::strtoll(row[0], nullptr, 10);
	}

	mysql_free_result(result);

	return value;
}

std::string CGuardStore::Quote(const std::string& value)
{
	std::string out(value.size() * 2 + 1, '\0');

	const unsigned long n = mysql_real_escape_string(Handle(this->m_Mysql), &out[0], value.c_str(), (unsigned long)value.size());

	out.resize(n);

	return "'" + out + "'";
}

void CGuardStore::LoadBans()
{
	if (mysql_query(Handle(this->m_Mysql),
		"SELECT `kind`, `value`, IFNULL(UNIX_TIMESTAMP(`expires_at`), 0) FROM `MuGuardBan`"
		" WHERE `lifted` = 0 AND `kind` IN ('machine', 'ip') AND (`expires_at` IS NULL OR `expires_at` > NOW())") != 0)
	{
		LogAdd(LOG_RED, "[MU-Guard] Error de la base: %s", mysql_error(Handle(this->m_Mysql)));

		return;
	}

	MYSQL_RES* result = mysql_store_result(Handle(this->m_Mysql));

	if (result == nullptr)
	{
		return;
	}

	std::map<std::string, std::int64_t> bans;

	while (MYSQL_ROW row = mysql_fetch_row(result))
	{
		if (row[0] == nullptr || row[1] == nullptr)
		{
			continue;
		}

		const std::string key = std::string(row[0]) + ":" + row[1];

		const std::int64_t until = row[2] != nullptr ? std::strtoll(row[2], nullptr, 10) : 0;

		// Dos bans del mismo: manda el más largo (0 = permanente).
		auto it = bans.find(key);

		if (it == bans.end() || (it->second != 0 && (until == 0 || until > it->second)))
		{
			bans[key] = until;
		}
	}

	mysql_free_result(result);

	this->m_Bans = bans;
}

void CGuardStore::Tick()
{
	if (Now() < this->m_NextBanLoad || !this->Ready())
	{
		return;
	}

	this->m_NextBanLoad = Now() + kBanReloadSeconds;

	this->LoadBans();
}

bool CGuardStore::IsBanned(const std::string& kind, const std::string& value, std::int64_t& until)
{
	if (value.empty())
	{
		return false;
	}

	auto it = this->m_Bans.find(kind + ":" + value);

	if (it == this->m_Bans.end())
	{
		return false;
	}

	if (it->second != 0 && it->second <= Now())
	{
		this->m_Bans.erase(it);

		return false;
	}

	until = it->second;

	return true;
}

std::int64_t CGuardStore::AddBan(const std::string& kind, const std::string& value, const std::string& reason, int amount, const char* unit)
{
	const std::string expires = amount == 0 ? std::string("NULL") : "NOW() + INTERVAL " + std::to_string(amount) + " " + unit;

	if (!this->Exec("INSERT INTO `MuGuardBan` (`kind`, `value`, `reason`, `expires_at`) VALUES (" + this->Quote(kind) + ", " +
		this->Quote(value) + ", " + this->Quote(reason) + ", " + expires + ")"))
	{
		return -1;
	}

	const std::int64_t until = amount == 0 ? 0 : this->Scalar("SELECT UNIX_TIMESTAMP(" + expires + ")", Now());

	if (kind != "account")
	{
		this->m_Bans[kind + ":" + value] = until;
	}

	return until;
}

GuardWarningResult CGuardStore::AddWarning(const GuardWarning& warning, const GuardSettings& settings)
{
	GuardWarningResult result;

	if (!this->Ready())
	{
		return result;
	}

	const std::string window = "NOW() - INTERVAL " + std::to_string(settings.WarningWindowDays) + " DAY";

	// Los avisos de antes del último ban de ese mismo no cuentan: al vencer, se
	// arranca de cero.
	const auto count = [&](const char* column, const char* kind, const std::string& value) -> int
	{
		if (value.empty())
		{
			return 0;
		}

		const std::string quoted = this->Quote(value);

		return (int)this->Scalar(std::string("SELECT COUNT(*) FROM `MuGuardWarning` WHERE `") + column + "` = " + quoted +
			" AND `voided` = 0 AND `created_at` > " + window +
			" AND `created_at` > IFNULL((SELECT MAX(`created_at`) FROM `MuGuardBan` WHERE `kind` = '" + kind + "' AND `value` = " + quoted + "), '1000-01-01')", 0);
	};

	const int byAccount = warning.account.empty() ? 0 : count("account", "account", warning.account) + 1;

	const int byMachine = warning.machine.empty() ? 0 : count("machine", "machine", warning.machine) + 1;

	result.number = byAccount > byMachine ? byAccount : byMachine;

	if (result.number == 0)
	{
		result.number = 1;
	}

	result.stored = this->Exec("INSERT INTO `MuGuardWarning` (`code`, `detail`, `account`, `character_name`, `machine`, `ip`, `build`, `number`) VALUES (" +
		this->Quote(GuardDetectionName(warning.code)) + ", " + this->Quote(warning.detail.substr(0, 512)) + ", " +
		this->Quote(warning.account) + ", " + this->Quote(warning.character) + ", " + this->Quote(warning.machine) + ", " +
		this->Quote(warning.ip) + ", " + this->Quote(warning.build) + ", " + std::to_string(result.number) + ")");

	const int limit = settings.WarningLimit;

	std::string lower = warning.account;

	for (char& ch : lower)
	{
		ch = (char)std::tolower((unsigned char)ch);
	}

	if (limit == 0 || !result.stored || Contains(settings.ExemptAccounts, lower))
	{
		return result;
	}

	const std::string reason = std::string(GuardDetectionName(warning.code)) + " (" + std::to_string(limit) + " avisos)";

	bool permanent = false;

	std::int64_t latest = 0;

	const auto note = [&](std::int64_t until)
	{
		if (until == 0)
		{
			permanent = true;
		}
		else if (until > latest)
		{
			latest = until;
		}
	};

	if (byAccount >= limit && settings.BanAccountDays >= 0)
	{
		const std::string expires = settings.BanAccountDays == 0 ? std::string("'2099-12-31 23:59:59'")
			: "NOW() + INTERVAL " + std::to_string(settings.BanAccountDays) + " DAY";

		// El ban de cuenta es el de siempre: lo levanta solo el JoinServer al vencer
		// (WZ_DesblocAccount) y el GameServer lo rechaza con el código 5.
		if (this->Exec("UPDATE `MEMB_INFO` SET `bloc_code` = '1', `Bloc_Expire` = " + expires + " WHERE `memb___id` = " + this->Quote(warning.account)))
		{
			const std::int64_t until = this->AddBan("account", warning.account, reason, settings.BanAccountDays, "DAY");

			if (until >= 0)
			{
				result.banned |= GuardProto::BanAccount;

				note(until);
			}
		}
	}

	if (byMachine >= limit && settings.BanMachineDays >= 0)
	{
		const std::int64_t until = this->AddBan("machine", warning.machine, reason, settings.BanMachineDays, "DAY");

		if (until >= 0)
		{
			result.banned |= GuardProto::BanMachine;

			note(until);
		}
	}

	// La IP no cuenta sola (un cyber, CGNAT): va con el ban de cuenta o de máquina.
	if (result.banned != 0 && settings.BanIpHours >= 0 && PublicIp(warning.ip) && !Contains(settings.ExemptIps, warning.ip))
	{
		const std::int64_t until = this->AddBan("ip", warning.ip, reason, settings.BanIpHours, "HOUR");

		if (until >= 0)
		{
			result.banned |= GuardProto::BanIp;
		}
	}

	result.until = permanent ? 0 : latest;

	return result;
}
