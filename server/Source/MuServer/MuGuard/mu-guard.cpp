// mu-guard: la parte de MU-Guard que corre en el server (Fase 5).  Diseño en
// doc-port/diseno-seguridad-fase5.md; mensajes en Common/GuardProtocol.h.
//
// Dos puertas:
//   - TCP en el puerto público (Guard.toml Port, 28980): el guardián del
//     cliente, por el mismo canal cifrado que el juego (SecureChannel) y con la
//     misma identidad del server (Data/Keys/ServerIdentity.key, la genera el
//     GameServer; acá sólo se lee).  Saluda y pide tickets.
//   - Socket Unix local (Guard.toml Socket): el GameServer.  Latido y
//     verificación de los tickets que llegan en el login.  El latido lleva de
//     vuelta los jugadores a desconectar.
//
// Desafío de memoria (paso 4): con el build del cliente en Data/Guard/Builds
// (lo arma `mu-guard build <ejecutable>`), cada ~30 s se le pide al guardián el
// hash de bloques del código del juego (una recorrida de todo el código en
// orden al azar).  Distinto del esperado: se le ordena cerrar el juego
// (Verdict PATCH) y se desconecta a sus jugadores.  Sin
// respuesta a tiempo, o con el guardián desconectado más de la gracia, se
// desconecta a sus jugadores.
//
// Avisos y bans (paso 5): una detección que cierra el juego (o un desafío
// fallado) suma un aviso en la base, uno por guardián; a los X avisos se banea
// cuenta, máquina e IP (GuardStore.h).  Una máquina o una IP baneadas no pasan
// el saludo.
//
// Listas y textos (paso 5c): Data/Guard/Lists.toml (herramientas y módulos) y
// Messages.toml (cómo se llama cada detección en el banner).  Se le mandan al
// guardián después del saludo, y de nuevo si cambian.
//
// Un solo hilo con poll(): son pocas conexiones y cada mensaje se atiende en
// microsegundos (las consultas a la base son pocas: un aviso es raro y los bans
// se leen de una caché).  Sólo Linux, como mu-trace.

#include "stdafx.h"
#include "GuardBuild.h"
#include "GuardConfig.h"
#include "GuardProtocol.h"
#include "GuardStore.h"
#include "SecureChannel.h"
#include "SecureRandom.h"
#include "ServerIdentity.h"

extern "C"
{
#include "monocypher/monocypher.h"
}

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

void LogAdd(int color, const char* text, ...)
{
	(void)color;

	char line[1024];

	va_list args;
	va_start(args, text);
	std::vsnprintf(line, sizeof(line), text, args);
	va_end(args);

	const std::time_t now = std::time(nullptr);

	char when[32];
	std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));

	std::printf("[%s] %s\n", when, line);
	std::fflush(stdout);
}

namespace
{
	using Clock = std::chrono::steady_clock;

	// Topes contra quien abuse del puerto público.  Un jugador usa una conexión
	// y un ticket por intento de login.
	constexpr int kMaxConnectionsPerIp = 8;
	constexpr int kMaxTicketsPerIpPerMinute = 30;
	constexpr int kHandshakeSeconds = 10;   // saludo del canal + Hello
	constexpr std::size_t kMaxTickets = 100000;
	constexpr std::size_t kMaxBuffered = 64 * 1024;
	// Cada desafío pide los próximos bloques de una recorrida al azar de todo el
	// código: 8 x 64 KB cada ~30 s cubren 16 MB en unos 16 minutos, así que un
	// parche que se queda en memoria cae seguro.
	constexpr std::size_t kChallengeRanges = GuardProto::MaxChallengeRanges;
	constexpr std::size_t kMaxSessionsPerGuardian = 64;
	constexpr std::size_t kMaxKicksPerPong = 256;
	constexpr int kKickSeconds = 15;   // lo que dura una orden de desconectar para un latido que se reconecta

	std::string Hex(const std::uint8_t* data, std::size_t size)
	{
		static const char kDigits[] = "0123456789abcdef";

		std::string text;

		for (std::size_t i = 0; i < size; i++)
		{
			text += kDigits[data[i] >> 4];
			text += kDigits[data[i] & 15];
		}

		return text;
	}

	// IP de la red del server: privada, loopback o de enlace local.  CGNAT
	// (100.64/10) no cuenta: ahí comparten IP clientes de afuera.
	bool LocalIp(const std::string& ip)
	{
		in_addr address {};

		if (::inet_pton(AF_INET, ip.c_str(), &address) != 1)
		{
			return false;
		}

		const std::uint32_t a = ntohl(address.s_addr);

		const auto in = [a](std::uint32_t net, int bits) { return (a >> (32 - bits)) == (net >> (32 - bits)); };

		return in(0x0A000000, 8) || in(0x7F000000, 8) || in(0xAC100000, 12) || in(0xC0A80000, 16) || in(0xA9FE0000, 16);
	}

	// Un número al azar en [low, high].
	int RandomBetween(int low, int high)
	{
		std::uint8_t b[4] = {};

		SecureRandomBytes(b, sizeof(b));

		return low + (int)(GuardProto::GetLe32(b) % (std::uint32_t)(high - low + 1));
	}

	std::string DataPath()
	{
		const char* env = std::getenv("MU_DATA_PATH");

		std::string path = (env != nullptr && env[0] != '\0') ? env : "./Data/";

		if (path.back() != '/')
		{
			path += '/';
		}

		return path;
	}

	void SetNonBlocking(int fd)
	{
		::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
	}

	struct Ticket
	{
		std::uint8_t key[GuardProto::TicketKeySize];
		std::string ip;
		std::string guardian;   // id del guardián que lo pidió
		Clock::time_point expires;
	};

	// Un guardián (puede reconectarse: se lo sigue por su id).  Sus sesiones son
	// los tickets que ya entraron al juego.
	struct GuardianState
	{
		int connections = 0;
		Clock::time_point lastSeen;
		std::set<std::string> sessions;
		std::string machine;     // hash de la máquina con la sal del server, en hex
		std::string ip;
		std::string build;       // primeros bytes del hash del ejecutable, en hex
		std::string account;     // los informa el GameServer (Identify)
		std::string character;
		bool warned = false;     // ya sumó su aviso (uno por guardián)
		std::vector<std::uint8_t> notice;   // la respuesta Notice de ese aviso
	};

	struct Kick
	{
		std::uint64_t serial;
		std::string ticket;
		Clock::time_point when;
	};

	struct Connection
	{
		int fd = -1;
		bool local = false;           // GameServer por el socket Unix
		std::string ip;
		Clock::time_point opened;
		bool greeted = false;         // el guardián mandó un Hello válido
		std::string guardian;         // su id (vacío hasta el Hello)
		std::shared_ptr<const CGuardBuild> build;   // nullptr: build desconocido, sin desafío
		bool condemned = false;       // falló un desafío: no se le piden más
		bool challenging = false;     // hay un desafío sin contestar
		std::uint32_t challenge = 0;
		std::uint8_t nonce[GuardProto::NonceSize] = {};
		std::vector<GuardBuildRange> ranges;
		std::vector<GuardBuildRange> tour;   // bloques del código en orden al azar
		std::size_t tourAt = 0;
		Clock::time_point deadline;   // del desafío en curso
		Clock::time_point nextChallenge;
		bool heartbeat = false;       // el GameServer mandó Ping por esta conexión
		std::uint64_t kickSerial = 0; // última orden de desconectar entregada
		std::unique_ptr<SecureChannel> channel;
		std::vector<std::uint8_t> in;  // texto claro sin procesar
		std::vector<std::uint8_t> out; // bytes para el socket
		bool closing = false;
	};

	class Guard
	{
	public:

		bool Start();

		void Run();

	private:

		void Accept(int listener, bool local);

		void Read(Connection& c);

		void Write(Connection& c);

		void Handle(Connection& c, std::uint8_t type, const std::vector<std::uint8_t>& payload);

		void Reply(Connection& c, std::uint8_t type, const void* data, std::size_t size);

		void HandleVerify(Connection& c, const std::vector<std::uint8_t>& payload);

		void HandleHello(Connection& c, const std::vector<std::uint8_t>& payload);

		void HandleAnswer(Connection& c, const std::vector<std::uint8_t>& payload);

		void SendChallenge(Connection& c);

		void SendPong(Connection& c);

		// Desconecta a los jugadores que entraron con un ticket de ese guardián.
		void KickGuardian(const std::string& guardian, const char* reason, const std::string& ip);

		// Una detección que cierra el juego: suma el aviso (si corresponde) y le
		// contesta al guardián con Notice.
		void Warn(Connection& c, int code, const std::string& detail);

		void HandleIdentify(const std::vector<std::uint8_t>& payload);

		// Sal del hash de las máquinas (Data/Guard/MachineSalt.key): así el hash
		// guardado no sirve para cruzar jugadores entre servers.
		bool LoadSalt();

		// Listas y textos (Rules) a un guardián.
		void SendRules(Connection& c);

		// El ticket se saludó desde una IP y el login llega desde otra, pero las
		// dos son de la red del server (ver Verify).
		bool SameNetwork(const std::string& ticketIp, const std::string& loginIp);

		void Drop(Connection& c);

		void LoadBuilds();

		void Tick();

		int ConnectionsFrom(const std::string& ip) const;

		std::string m_Data;

		GuardSettings m_Settings;

		int m_Tcp = -1;

		int m_Unix = -1;

		std::vector<std::unique_ptr<Connection>> m_Connections;

		std::map<std::string, Ticket> m_Tickets;   // id en binario -> ticket

		std::map<std::string, std::vector<Clock::time_point>> m_Issued;   // IP -> tickets del último minuto

		std::map<std::string, GuardianState> m_Guardians;   // id del guardián

		std::map<std::string, std::string> m_SessionOwner;   // id del ticket -> id del guardián

		CGuardStore m_Store;

		std::uint8_t m_Salt[32] = {};

		GuardRules m_Rules;

		// IPs de PUBLIC_IP (puede ser un nombre), resueltas cuando hacen falta.
		std::vector<std::string> m_PublicIps;

		Clock::time_point m_PublicIpsAt {};

		std::map<std::string, std::shared_ptr<const CGuardBuild>> m_Builds;   // hash del ejecutable

		std::map<std::string, std::filesystem::file_time_type> m_BuildFiles;   // para releer si cambian

		std::deque<Kick> m_Kicks;

		std::uint64_t m_KickSerial = 0;

		std::uint32_t m_ChallengeSerial = 0;

		Clock::time_point m_NextTick;

		Clock::time_point m_NextBuildScan;
	};

	void Guard::LoadBuilds()
	{
		const std::string folder = this->m_Data + "Guard/Builds";

		std::map<std::string, std::filesystem::file_time_type> files;

		std::error_code error;

		for (std::filesystem::directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
		{
			if (it->path().extension() == ".guardbuild")
			{
				files[it->path().string()] = it->last_write_time(error);
			}
		}

		if (files == this->m_BuildFiles)
		{
			return;
		}

		this->m_BuildFiles = files;

		std::map<std::string, std::shared_ptr<const CGuardBuild>> builds;

		for (const auto& file : files)
		{
			auto build = std::make_shared<CGuardBuild>();

			if (!build->Load(file.first))
			{
				LogAdd(LOG_RED, "[MU-Guard] Build ilegible: %s", file.first.c_str());

				continue;
			}

			builds[std::string((const char*)build->Hash(), GuardProto::HashSize)] = build;
		}

		this->m_Builds = builds;

		LogAdd(LOG_BLUE, "[MU-Guard] Builds aceptados: %d (Data/Guard/Builds)", (int)builds.size());
	}

	bool Guard::LoadSalt()
	{
		const std::string path = this->m_Data + "Guard/MachineSalt.key";

		if (FILE* file = std::fopen(path.c_str(), "rb"))
		{
			const std::size_t n = std::fread(this->m_Salt, 1, sizeof(this->m_Salt), file);

			std::fclose(file);

			if (n == sizeof(this->m_Salt))
			{
				return true;
			}
		}

		if (!SecureRandomBytes(this->m_Salt, sizeof(this->m_Salt)))
		{
			return false;
		}

		FILE* file = std::fopen(path.c_str(), "wb");

		if (file == nullptr)
		{
			LogAdd(LOG_RED, "[MU-Guard] No se pudo crear %s", path.c_str());

			return false;
		}

		const bool ok = std::fwrite(this->m_Salt, 1, sizeof(this->m_Salt), file) == sizeof(this->m_Salt);

		std::fclose(file);

		::chmod(path.c_str(), 0600);

		return ok;
	}

	bool Guard::Start()
	{
		this->m_Data = DataPath();

		this->m_Settings = ReadGuardSettings(this->m_Data);

		if (!this->LoadSalt())
		{
			LogAdd(LOG_RED, "[MU-Guard] Sin sal para las maquinas: no se puede arrancar");

			return false;
		}

		this->m_Store.Tick();

		this->m_Rules = ReadGuardRules(this->m_Data);

		LogAdd(LOG_BLUE, "[MU-Guard] Listas: %d procesos, %d ventanas, %d modulos bloqueados, %d permitidos, %d textos",
			(int)this->m_Rules.Processes.size(), (int)this->m_Rules.Windows.size(), (int)this->m_Rules.ModuleBlock.size(),
			(int)this->m_Rules.ModuleAllow.size(), (int)this->m_Rules.Names.size());

		gServerIdentity.Reload((this->m_Data + "Keys").c_str());

		if (!gServerIdentity.IsLoaded())
		{
			LogAdd(LOG_RED, "[MU-Guard] Todavia no esta la clave del server (Data/Keys/ServerIdentity.key): se espera a que la genere el GameServer");
		}

		this->m_Tcp = ::socket(AF_INET, SOCK_STREAM, 0);

		const int on = 1;

		::setsockopt(this->m_Tcp, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

		sockaddr_in address {};
		address.sin_family = AF_INET;
		address.sin_addr.s_addr = htonl(INADDR_ANY);
		address.sin_port = htons(this->m_Settings.Port);

		if (::bind(this->m_Tcp, (sockaddr*)&address, sizeof(address)) != 0 || ::listen(this->m_Tcp, 64) != 0)
		{
			LogAdd(LOG_RED, "[MU-Guard] No se pudo escuchar en el puerto %d: %s", (int)this->m_Settings.Port, std::strerror(errno));

			return false;
		}

		SetNonBlocking(this->m_Tcp);

		// El socket local: un archivo que quedó de una corrida anterior se borra.
		const std::string& path = this->m_Settings.Socket;

		sockaddr_un local {};
		local.sun_family = AF_UNIX;

		if (path.size() >= sizeof(local.sun_path))
		{
			LogAdd(LOG_RED, "[MU-Guard] La ruta del socket local es demasiado larga: %s", path.c_str());

			return false;
		}

		std::memcpy(local.sun_path, path.c_str(), path.size() + 1);

		::unlink(path.c_str());

		this->m_Unix = ::socket(AF_UNIX, SOCK_STREAM, 0);

		if (::bind(this->m_Unix, (sockaddr*)&local, sizeof(local)) != 0 || ::listen(this->m_Unix, 16) != 0)
		{
			LogAdd(LOG_RED, "[MU-Guard] No se pudo abrir el socket local %s: %s", path.c_str(), std::strerror(errno));

			return false;
		}

		// Sólo el usuario del server (el GameServer corre con el mismo).
		::chmod(path.c_str(), 0600);

		SetNonBlocking(this->m_Unix);

		LogAdd(LOG_BLUE, "[MU-Guard] Escuchando en el puerto %d y en %s (%s)", (int)this->m_Settings.Port, path.c_str(),
			this->m_Settings.Required ? "exige el ticket" : "no lo exige");

		this->LoadBuilds();

		this->m_NextTick = Clock::now();

		this->m_NextBuildScan = Clock::now() + std::chrono::seconds(10);

		return true;
	}

	int Guard::ConnectionsFrom(const std::string& ip) const
	{
		int count = 0;

		for (const auto& c : this->m_Connections)
		{
			if (!c->local && c->ip == ip)
			{
				count++;
			}
		}

		return count;
	}

	void Guard::Accept(int listener, bool local)
	{
		for (;;)
		{
			sockaddr_storage from {};
			socklen_t length = sizeof(from);

			const int fd = ::accept(listener, (sockaddr*)&from, &length);

			if (fd < 0)
			{
				return;
			}

			SetNonBlocking(fd);

			auto c = std::make_unique<Connection>();
			c->fd = fd;
			c->local = local;
			c->opened = Clock::now();

			if (local)
			{
				c->ip = "local";
			}
			else
			{
				char text[INET6_ADDRSTRLEN] = {};

				if (from.ss_family == AF_INET)
				{
					::inet_ntop(AF_INET, &((sockaddr_in*)&from)->sin_addr, text, sizeof(text));
				}

				c->ip = text;

				const int on = 1;

				::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

				BYTE random[SecureChannel::KeySize];

				if (this->ConnectionsFrom(c->ip) >= kMaxConnectionsPerIp || !gServerIdentity.IsLoaded() || !SecureRandomBytes(random, sizeof(random)))
				{
					::close(fd);

					continue;
				}

				c->channel = std::make_unique<SecureChannel>();

				BYTE hello[SecureChannel::ServerHelloSize];

				const bool rotation = gServerIdentity.HasRotation();

				c->channel->BeginServer(hello, random, gServerIdentity.SecretKey(), gServerIdentity.PublicKey(),
					rotation ? gServerIdentity.PreviousPublicKey() : nullptr,
					rotation ? gServerIdentity.RotationSignature() : nullptr);

				crypto_wipe(random, sizeof(random));

				c->out.assign(hello, hello + sizeof(hello));
			}

			this->m_Connections.push_back(std::move(c));
		}
	}

	void Guard::Reply(Connection& c, std::uint8_t type, const void* data, std::size_t size)
	{
		std::vector<std::uint8_t> message;

		GuardProto::Append(message, type, data, size);

		if (c.local)
		{
			c.out.insert(c.out.end(), message.begin(), message.end());

			return;
		}

		std::vector<std::uint8_t> sealed(message.size() + SecureChannel::Overhead);

		const std::size_t n = c.channel->Seal(message.data(), message.size(), sealed.data());

		if (n == 0)
		{
			c.closing = true;

			return;
		}

		c.out.insert(c.out.end(), sealed.begin(), sealed.begin() + (std::ptrdiff_t)n);
	}

	void Guard::Read(Connection& c)
	{
		std::uint8_t buffer[4096];

		for (;;)
		{
			const ssize_t n = ::recv(c.fd, buffer, sizeof(buffer), 0);

			if (n == 0)
			{
				c.closing = true;

				return;
			}

			if (n < 0)
			{
				if (errno != EAGAIN && errno != EWOULDBLOCK)
				{
					c.closing = true;
				}

				break;
			}

			if (c.local)
			{
				c.in.insert(c.in.end(), buffer, buffer + n);
			}
			else
			{
				if (!c.channel->Receive(buffer, (std::size_t)n, c.in))
				{
					c.closing = true;

					return;
				}

				// El ClientHello no lleva respuesta; si el canal la pidiera, sale en claro.
				BYTE reply[SecureChannel::ClientHelloSize];

				const std::size_t r = c.channel->TakeReply(reply);

				c.out.insert(c.out.end(), reply, reply + r);
			}

			if (c.in.size() > kMaxBuffered)
			{
				c.closing = true;

				return;
			}
		}

		std::uint8_t type = 0;

		std::vector<std::uint8_t> payload;

		for (;;)
		{
			const int got = GuardProto::Pop(c.in, type, payload);

			if (got < 0)
			{
				c.closing = true;

				return;
			}

			if (got == 0)
			{
				break;
			}

			this->Handle(c, type, payload);

			if (c.closing)
			{
				return;
			}
		}
	}

	void Guard::Handle(Connection& c, std::uint8_t type, const std::vector<std::uint8_t>& payload)
	{
		using namespace GuardProto;

		if (c.local)
		{
			if (type == Ping)
			{
				c.heartbeat = true;

				this->SendPong(c);
			}
			else if (type == Verify)
			{
				this->HandleVerify(c, payload);
			}
			else if (type == Identify)
			{
				this->HandleIdentify(payload);
			}
			else
			{
				c.closing = true;
			}

			return;
		}

		if (type == Hello && !c.greeted)
		{
			this->HandleHello(c, payload);

			return;
		}

		if (type == ChallengeAnswer && c.greeted)
		{
			this->HandleAnswer(c, payload);

			return;
		}

		if (type == TicketRequest)
		{
			std::uint8_t reply[TicketSize] = {};

			reply[0] = NotReady;

			auto& issued = this->m_Issued[c.ip];

			const Clock::time_point now = Clock::now();

			while (!issued.empty() && now - issued.front() > std::chrono::minutes(1))
			{
				issued.erase(issued.begin());
			}

			if (c.greeted && (int)issued.size() < kMaxTicketsPerIpPerMinute && this->m_Tickets.size() < kMaxTickets)
			{
				Ticket ticket;

				std::uint8_t id[TicketIdSize];

				if (SecureRandomBytes(id, sizeof(id)) && SecureRandomBytes(ticket.key, sizeof(ticket.key)))
				{
					ticket.ip = c.ip;

					ticket.guardian = c.guardian;

					ticket.expires = now + std::chrono::seconds(TicketSeconds);

					this->m_Tickets[std::string((const char*)id, sizeof(id))] = ticket;

					issued.push_back(now);

					reply[0] = Ok;

					std::memcpy(reply + 1, id, sizeof(id));

					std::memcpy(reply + 1 + TicketIdSize, ticket.key, TicketKeySize);
				}
			}

			this->Reply(c, GuardProto::Ticket, reply, sizeof(reply));

			crypto_wipe(reply, sizeof(reply));

			return;
		}

		if (type == Report && c.greeted && payload.size() >= 2 && payload.size() <= 2 + MaxDetail)
		{
			// Todo se registra; lo que cerró el juego suma además un aviso.
			const char* name = GuardDetectionName(payload[0]);
			std::string detail(payload.begin() + 2, payload.end());

			for (char& ch : detail)
			{
				if ((unsigned char)ch < 0x20)
				{
					ch = ' ';   // el detalle lo escribe el cliente: sin saltos de línea en el log
				}
			}

			LogAdd(payload[1] ? LOG_RED : LOG_BLUE, "[MU-Guard] [%s] %s %s: %s", c.ip.c_str(), name,
				payload[1] ? "(cerro el juego)" : "(registrado)", detail.c_str());

			if (payload[1])
			{
				this->Warn(c, payload[0], detail);
			}

			return;
		}

		// Cualquier otra cosa del guardián: fuera.
		c.closing = true;
	}

	void Guard::HandleVerify(Connection& c, const std::vector<std::uint8_t>& payload)
	{
		using namespace GuardProto;

		std::uint8_t reply[VerifyResultSize] = {};

		if (payload.size() != VerifySize)
		{
			reply[4] = Malformed;

			this->Reply(c, VerifyResult, reply, sizeof(reply));

			return;
		}

		const std::uint8_t* p = payload.data();

		std::memcpy(reply, p, 4);   // número de pedido

		const std::string id((const char*)p + 4, TicketIdSize);

		const std::uint8_t* mac = p + 4 + TicketIdSize;

		const std::uint8_t* transcript = mac + MacSize;

		char ip[IpSize + 1] = {};

		std::memcpy(ip, transcript + TranscriptSize, IpSize);

		auto it = this->m_Tickets.find(id);

		std::int64_t until = 0;

		if (this->m_Store.IsBanned("ip", ip, until))
		{
			if (it != this->m_Tickets.end())
			{
				this->m_Tickets.erase(it);
			}

			reply[4] = BannedIp;
		}
		else if (it == this->m_Tickets.end())
		{
			// Un id ya usado tampoco está: se borra al presentarlo.  Un id en ceros
			// es un login sin ticket: sólo se preguntaba por la IP.
			reply[4] = UnknownTicket;
		}
		else
		{
			const Ticket ticket = it->second;

			// Un solo intento por ticket, salga bien o mal.
			this->m_Tickets.erase(it);

			std::uint8_t message[sizeof(TicketContext) - 1 + TranscriptSize];

			std::memcpy(message, TicketContext, sizeof(TicketContext) - 1);

			std::memcpy(message + sizeof(TicketContext) - 1, transcript, TranscriptSize);

			std::uint8_t expected[MacSize];

			crypto_blake2b_keyed(expected, sizeof(expected), ticket.key, sizeof(ticket.key), message, sizeof(message));

			if (Clock::now() > ticket.expires)
			{
				reply[4] = Expired;
			}
			else if (ticket.ip != ip && !this->SameNetwork(ticket.ip, ip))
			{
				reply[4] = WrongIp;
			}
			else if (crypto_verify32(expected, mac) != 0)
			{
				reply[4] = WrongMac;
			}
			else
			{
				reply[4] = Valid;

				// Desde acá el jugador queda atado a su guardián: si el guardián falla
				// un desafío o desaparece, se lo desconecta.
				GuardianState& guardian = this->m_Guardians[ticket.guardian];

				if (guardian.connections == 0 && guardian.sessions.empty())
				{
					guardian.lastSeen = Clock::now();
				}

				if (guardian.sessions.size() < kMaxSessionsPerGuardian)
				{
					guardian.sessions.insert(id);

					this->m_SessionOwner[id] = ticket.guardian;
				}
			}

			crypto_wipe(expected, sizeof(expected));
		}

		if (reply[4] == BannedIp)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Login rechazado: IP baneada", ip);
		}
		else if (reply[4] != Valid && std::any_of(id.begin(), id.end(), [](char ch) { return ch != 0; }))
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Ticket rechazado en el login (motivo %d)", ip, (int)reply[4]);
		}

		this->Reply(c, VerifyResult, reply, sizeof(reply));
	}

	void Guard::HandleHello(Connection& c, const std::vector<std::uint8_t>& payload)
	{
		using namespace GuardProto;

		std::uint8_t result = Ok;

		std::shared_ptr<const CGuardBuild> build;

		if (payload.size() != HelloSize || (payload[0] | (payload[1] << 8)) != Version)
		{
			result = BadVersion;
		}
		else
		{
			auto it = this->m_Builds.find(std::string((const char*)payload.data() + 2, HashSize));

			if (it != this->m_Builds.end())
			{
				build = it->second;
			}
			else if (!this->m_Settings.AcceptUnknownBuilds)
			{
				result = UnknownBuild;
			}
		}

		// La máquina: el hash del guardián con la sal del server.
		std::string machine;

		std::uint8_t banned = 0;

		std::int64_t until = 0;

		if (result != BadVersion)
		{
			std::uint8_t salted[16];

			crypto_blake2b_keyed(salted, sizeof(salted), this->m_Salt, sizeof(this->m_Salt), payload.data() + 2 + HashSize + GuardianIdSize, MachineSize);

			machine = Hex(salted, sizeof(salted));

			std::int64_t when = 0;

			if (this->m_Store.IsBanned("machine", machine, when))
			{
				banned |= BanMachine;

				until = when;
			}

			if (this->m_Store.IsBanned("ip", c.ip, when))
			{
				banned |= BanIp;

				until = (until == 0 && (banned & BanMachine)) ? 0 : (when == 0 || when > until ? when : until);
			}

			if (banned != 0)
			{
				result = Banned;
			}
		}

		const std::uint8_t options = static_cast<std::uint8_t>((this->m_Settings.MemScanCloses ? MemScanCloses : 0) | (this->m_Settings.ToolCloses ? ToolCloses : 0));

		std::uint8_t reply[HelloResultBannedSize] = { result, options };

		if (result == Banned)
		{
			reply[2] = banned;

			PutLe64(reply + 3, (std::uint64_t)until);
		}

		this->Reply(c, HelloResult, reply, result == Banned ? sizeof(reply) : 2);

		if (result == BadVersion)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Guardian de otra version: rechazado", c.ip.c_str());

			return;
		}

		if (result == Banned)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] maquina=%s baneada (%s%s): no se abre el juego", c.ip.c_str(), machine.c_str(),
				(banned & BanMachine) ? "maquina" : "", (banned & BanIp) ? ((banned & BanMachine) ? " e IP" : "IP") : "");

			return;
		}

		const std::string hash = Hex(payload.data() + 2, 8);

		if (result == UnknownBuild)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Cliente %s... no esta entre los builds aceptados: rechazado", c.ip.c_str(), hash.c_str());

			return;
		}

		c.greeted = true;

		c.guardian.assign((const char*)payload.data() + 2 + HashSize, GuardianIdSize);

		c.build = build;

		GuardianState& guardian = this->m_Guardians[c.guardian];

		guardian.connections++;

		guardian.lastSeen = Clock::now();

		guardian.machine = machine;

		guardian.ip = c.ip;

		guardian.build = Hex(payload.data() + 2, 8);

		this->SendRules(c);

		if (build)
		{
			// El primero cuando el juego ya está andando.
			c.nextChallenge = Clock::now() + std::chrono::seconds(RandomBetween(15, 30));
		}
		else
		{
			LogAdd(LOG_BLUE, "[MU-Guard] [%s] Cliente %s... sin build registrado: entra sin desafio de memoria", c.ip.c_str(), hash.c_str());
		}
	}

	void Guard::SendChallenge(Connection& c)
	{
		using namespace GuardProto;

		if (!SecureRandomBytes(c.nonce, sizeof(c.nonce)))
		{
			return;
		}

		c.ranges.clear();

		while (c.ranges.size() < kChallengeRanges)
		{
			if (c.tourAt >= c.tour.size())
			{
				// Una recorrida nueva, en otro orden.
				c.tour = c.build->Chunks(MaxChallengeRange);

				for (std::size_t i = c.tour.size(); i > 1; i--)
				{
					std::swap(c.tour[i - 1], c.tour[(std::size_t)RandomBetween(0, (int)i - 1)]);
				}

				c.tourAt = 0;

				if (c.tour.empty())
				{
					return;
				}
			}

			c.ranges.push_back(c.tour[c.tourAt++]);
		}

		c.challenge = ++this->m_ChallengeSerial;

		c.challenging = true;

		c.deadline = Clock::now() + std::chrono::seconds(ChallengeTimeoutSeconds);

		std::vector<std::uint8_t> body(4 + NonceSize + 1 + c.ranges.size() * 8);

		PutLe32(body.data(), c.challenge);

		std::memcpy(body.data() + 4, c.nonce, NonceSize);

		body[4 + NonceSize] = (std::uint8_t)c.ranges.size();

		for (std::size_t i = 0; i < c.ranges.size(); i++)
		{
			PutLe32(body.data() + 5 + NonceSize + i * 8, c.ranges[i].Rva);

			PutLe32(body.data() + 9 + NonceSize + i * 8, c.ranges[i].Size);
		}

		this->Reply(c, Challenge, body.data(), body.size());
	}

	void Guard::HandleAnswer(Connection& c, const std::vector<std::uint8_t>& payload)
	{
		using namespace GuardProto;

		if (payload.size() != ChallengeAnswerSize)
		{
			c.closing = true;

			return;
		}

		// Respuesta tardía de un desafío ya vencido, o sin desafío en curso.
		if (!c.challenging || GetLe32(payload.data()) != c.challenge)
		{
			return;
		}

		c.challenging = false;

		const std::uint64_t base = GetLe64(payload.data() + 4);

		std::uint8_t expected[HashSize];

		const bool known = c.build->Expected(c.nonce, c.ranges, base, expected);

		if (known && crypto_verify32(expected, payload.data() + 12) == 0)
		{
			const int every = this->m_Settings.ChallengeSeconds;

			c.nextChallenge = Clock::now() + std::chrono::seconds(RandomBetween(every - every / 3, every + every / 3));

			return;
		}

		c.condemned = true;

		const char detail[] = "código del juego modificado en memoria";

		std::vector<std::uint8_t> verdict{ Patch };

		verdict.insert(verdict.end(), detail, detail + sizeof(detail) - 1);

		this->Reply(c, Verdict, verdict.data(), verdict.size());

		this->Warn(c, Patch, detail);

		this->KickGuardian(c.guardian, "PATCH (fallo el desafio de memoria)", c.ip);
	}

	void Guard::Warn(Connection& c, int code, const std::string& detail)
	{
		using namespace GuardProto;

		GuardianState& guardian = this->m_Guardians[c.guardian];

		if (!guardian.warned)
		{
			guardian.warned = true;

			std::uint8_t notice[NoticeSize] = {};

			if ((this->m_Settings.WarningCodes & (1u << code)) != 0)
			{
				GuardWarning warning;
				warning.code = code;
				warning.detail = detail;
				warning.account = guardian.account;
				warning.character = guardian.character;
				warning.machine = guardian.machine;
				warning.ip = c.ip;
				warning.build = guardian.build;

				const GuardWarningResult result = this->m_Store.AddWarning(warning, this->m_Settings);

				const int limit = this->m_Settings.WarningLimit;

				LogAdd(LOG_RED, "[MU-Guard] %s cuenta=%s pj=%s ip=%s maquina=%s aviso=%d/%d%s%s", GuardDetectionName(code),
					warning.account.empty() ? "-" : warning.account.c_str(), warning.character.empty() ? "-" : warning.character.c_str(),
					c.ip.c_str(), warning.machine.c_str(), result.number, limit,
					result.stored ? "" : " (sin base: no quedo guardado)",
					result.banned != 0 ? " -> BANEADO" : "");

				if (result.stored)
				{
					notice[0] = (std::uint8_t)(result.number > 255 ? 255 : result.number);

					notice[1] = (std::uint8_t)(limit > 255 ? 255 : limit);

					notice[2] = result.banned;

					PutLe64(notice + 3, (std::uint64_t)result.until);
				}
			}

			guardian.notice.assign(notice, notice + sizeof(notice));
		}

		this->Reply(c, Notice, guardian.notice.data(), guardian.notice.size());
	}

	// Un jugador en la misma red que el server: según el puerto, el router le
	// deja la IP privada o le pone la pública del server (cuando se entra por
	// el dominio desde adentro), así que el saludo y el login pueden llegar con
	// IPs distintas.  Se acepta sólo si el login viene de la red local y el
	// saludo también, o de la IP pública del server.  Entre IPs de afuera el
	// ticket sigue atado a la suya.
	bool Guard::SameNetwork(const std::string& ticketIp, const std::string& loginIp)
	{
		if (!LocalIp(loginIp))
		{
			return false;
		}

		if (LocalIp(ticketIp))
		{
			return true;
		}

		const char* name = std::getenv("PUBLIC_IP");

		if (name == nullptr || *name == '\0')
		{
			return false;
		}

		if (this->m_PublicIps.empty() || Clock::now() - this->m_PublicIpsAt > std::chrono::minutes(5))
		{
			this->m_PublicIps.clear();

			this->m_PublicIpsAt = Clock::now();

			addrinfo hints {};

			hints.ai_family = AF_INET;

			hints.ai_socktype = SOCK_STREAM;

			addrinfo* list = nullptr;

			if (::getaddrinfo(name, nullptr, &hints, &list) == 0)
			{
				for (addrinfo* entry = list; entry != nullptr; entry = entry->ai_next)
				{
					char text[INET_ADDRSTRLEN] = {};

					::inet_ntop(AF_INET, &((sockaddr_in*)entry->ai_addr)->sin_addr, text, sizeof(text));

					this->m_PublicIps.push_back(text);
				}

				::freeaddrinfo(list);
			}
		}

		return std::find(this->m_PublicIps.begin(), this->m_PublicIps.end(), ticketIp) != this->m_PublicIps.end();
	}

	void Guard::SendRules(Connection& c)
	{
		using namespace GuardProto;

		const std::uint8_t reset = RuleReset;

		this->Reply(c, Rules, &reset, 1);

		std::size_t sent = 0;

		const auto send = [&](std::uint8_t kind, const std::vector<std::string>& list)
		{
			for (const std::string& text : list)
			{
				if (sent++ >= MaxRules)
				{
					return;
				}

				std::vector<std::uint8_t> body{ kind };

				body.insert(body.end(), text.begin(), text.begin() + (std::ptrdiff_t)std::min(text.size(), MaxRule));

				this->Reply(c, Rules, body.data(), body.size());
			}
		};

		send(RuleProcess, this->m_Rules.Processes);
		send(RuleWindow, this->m_Rules.Windows);
		send(RuleModuleBlock, this->m_Rules.ModuleBlock);
		send(RuleModuleAllow, this->m_Rules.ModuleAllow);
		send(RuleName, this->m_Rules.Names);
	}

	void Guard::HandleIdentify(const std::vector<std::uint8_t>& payload)
	{
		using namespace GuardProto;

		if (payload.size() != IdentifySize)
		{
			return;
		}

		auto owner = this->m_SessionOwner.find(std::string((const char*)payload.data(), TicketIdSize));

		if (owner == this->m_SessionOwner.end())
		{
			return;
		}

		auto it = this->m_Guardians.find(owner->second);

		if (it == this->m_Guardians.end())
		{
			return;
		}

		const auto field = [&](std::size_t at) {
			const char* p = (const char*)payload.data() + at;
			return std::string(p, strnlen(p, AccountSize));
		};

		const std::string account = field(TicketIdSize);

		const std::string character = field(TicketIdSize + AccountSize);

		if (!account.empty())
		{
			it->second.account = account;
		}

		it->second.character = character;
	}

	void Guard::KickGuardian(const std::string& guardian, const char* reason, const std::string& ip)
	{
		auto it = this->m_Guardians.find(guardian);

		const int count = it == this->m_Guardians.end() ? 0 : (int)it->second.sessions.size();

		LogAdd(LOG_RED, "[MU-Guard] [%s] %s: se desconecta a %d jugador(es)", ip.c_str(), reason, count);

		if (it == this->m_Guardians.end())
		{
			return;
		}

		for (const std::string& ticket : it->second.sessions)
		{
			this->m_Kicks.push_back({ ++this->m_KickSerial, ticket, Clock::now() });

			this->m_SessionOwner.erase(ticket);
		}

		it->second.sessions.clear();
	}

	void Guard::SendPong(Connection& c)
	{
		std::vector<std::uint8_t> body(2);

		std::uint16_t count = 0;

		for (const Kick& kick : this->m_Kicks)
		{
			if (kick.serial <= c.kickSerial || count >= kMaxKicksPerPong)
			{
				continue;
			}

			body.insert(body.end(), kick.ticket.begin(), kick.ticket.end());

			c.kickSerial = kick.serial;

			count++;
		}

		body[0] = (std::uint8_t)(count & 0xFF);

		body[1] = (std::uint8_t)(count >> 8);

		this->Reply(c, GuardProto::Pong, body.data(), body.size());
	}

	void Guard::Drop(Connection& c)
	{
		if (c.local || !c.greeted)
		{
			return;
		}

		auto it = this->m_Guardians.find(c.guardian);

		if (it != this->m_Guardians.end())
		{
			it->second.connections--;

			it->second.lastSeen = Clock::now();
		}
	}

	void Guard::Write(Connection& c)
	{
		while (!c.out.empty())
		{
			const ssize_t n = ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);

			if (n <= 0)
			{
				if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				{
					c.closing = true;
				}

				return;
			}

			c.out.erase(c.out.begin(), c.out.begin() + n);
		}
	}

	void Guard::Tick()
	{
		const Clock::time_point now = Clock::now();

		for (auto it = this->m_Tickets.begin(); it != this->m_Tickets.end();)
		{
			it = (now > it->second.expires) ? this->m_Tickets.erase(it) : std::next(it);
		}

		for (auto it = this->m_Issued.begin(); it != this->m_Issued.end();)
		{
			auto& list = it->second;

			while (!list.empty() && now - list.front() > std::chrono::minutes(1))
			{
				list.erase(list.begin());
			}

			it = list.empty() ? this->m_Issued.erase(it) : std::next(it);
		}

		// Quien no termina el saludo y el Hello a tiempo, afuera.
		for (const auto& c : this->m_Connections)
		{
			if (!c->local && !c->greeted && now - c->opened > std::chrono::seconds(kHandshakeSeconds))
			{
				c->closing = true;
			}
		}

		// Desafíos: vencidos y nuevos.
		for (const auto& c : this->m_Connections)
		{
			if (c->local || !c->greeted || !c->build || c->condemned || c->closing)
			{
				continue;
			}

			if (c->challenging && now > c->deadline)
			{
				c->challenging = false;

				c->condemned = true;

				c->closing = true;

				this->KickGuardian(c->guardian, "El guardian no contesto el desafio de memoria", c->ip);
			}
			else if (!c->challenging && now >= c->nextChallenge)
			{
				this->SendChallenge(*c);
			}
		}

		// Guardianes que se fueron: pasada la gracia, sus jugadores afuera.
		for (auto it = this->m_Guardians.begin(); it != this->m_Guardians.end();)
		{
			GuardianState& guardian = it->second;

			if (guardian.connections > 0 || now - guardian.lastSeen < std::chrono::seconds(GuardProto::GuardianGraceSeconds))
			{
				++it;

				continue;
			}

			if (!guardian.sessions.empty())
			{
				this->KickGuardian(it->first, "El guardian se desconecto", guardian.ip.empty() ? "-" : guardian.ip);
			}

			it = this->m_Guardians.erase(it);
		}

		while (!this->m_Kicks.empty() && now - this->m_Kicks.front().when > std::chrono::seconds(kKickSeconds))
		{
			this->m_Kicks.pop_front();
		}

		if (now >= this->m_NextBuildScan)
		{
			this->LoadBuilds();

			this->m_NextBuildScan = now + std::chrono::seconds(10);
		}

		// La clave la rota el GameServer: se relee si cambió.
		if (gServerIdentity.Reload((this->m_Data + "Keys").c_str()))
		{
			LogAdd(LOG_BLUE, "[MU-Guard] Clave del server cargada");
		}

		this->m_Settings = ReadGuardSettings(this->m_Data);

		this->m_Store.Tick();

		// Listas o textos nuevos: a todos los guardianes conectados.
		const GuardRules rules = ReadGuardRules(this->m_Data);

		if (rules.Version != this->m_Rules.Version)
		{
			const bool first = this->m_Rules.Version == 0;

			this->m_Rules = rules;

			LogAdd(LOG_BLUE, "[MU-Guard] Listas: %d procesos, %d ventanas, %d modulos bloqueados, %d permitidos, %d textos",
				(int)rules.Processes.size(), (int)rules.Windows.size(), (int)rules.ModuleBlock.size(), (int)rules.ModuleAllow.size(), (int)rules.Names.size());

			for (const auto& c : this->m_Connections)
			{
				if (!first && !c->local && c->greeted && !c->closing)
				{
					this->SendRules(*c);
				}
			}
		}
	}

	void Guard::Run()
	{
		for (;;)
		{
			std::vector<pollfd> fds;

			fds.push_back({ this->m_Tcp, POLLIN, 0 });

			fds.push_back({ this->m_Unix, POLLIN, 0 });

			for (const auto& c : this->m_Connections)
			{
				fds.push_back({ c->fd, (short)(POLLIN | (c->out.empty() ? 0 : POLLOUT)), 0 });
			}

			::poll(fds.data(), fds.size(), 500);

			if (fds[0].revents & POLLIN)
			{
				this->Accept(this->m_Tcp, false);
			}

			if (fds[1].revents & POLLIN)
			{
				this->Accept(this->m_Unix, true);
			}

			// Las conexiones nuevas quedaron al final, fuera de `fds`.
			for (std::size_t n = 2; n < fds.size(); n++)
			{
				Connection& c = *this->m_Connections[n - 2];

				if (fds[n].revents & (POLLIN | POLLHUP | POLLERR))
				{
					this->Read(c);
				}
			}

			for (const auto& c : this->m_Connections)
			{
				this->Write(*c);
			}

			if (Clock::now() >= this->m_NextTick)
			{
				this->Tick();

				this->m_NextTick = Clock::now() + std::chrono::seconds(1);
			}

			for (auto it = this->m_Connections.begin(); it != this->m_Connections.end();)
			{
				if ((*it)->closing)
				{
					this->Drop(**it);

					::close((*it)->fd);

					it = this->m_Connections.erase(it);
				}
				else
				{
					++it;
				}
			}
		}
	}
}

int main(int argc, char** argv)
{
	// mu-guard build <ejecutable> [salida]: el manifiesto de un build del
	// cliente, para Data/Guard/Builds.
	if (argc >= 3 && std::strcmp(argv[1], "build") == 0)
	{
		CGuardBuild build;

		std::string error;

		if (!build.FromExecutable(argv[2], &error))
		{
			std::fprintf(stderr, "%s: %s\n", argv[2], error.c_str());

			return 1;
		}

		std::string out = argc >= 4 ? argv[3] : std::string();

		if (out.empty())
		{
			out = std::filesystem::path(argv[2]).filename().string() + ".guardbuild";
		}

		if (!build.Save(out))
		{
			std::fprintf(stderr, "No se pudo escribir %s\n", out.c_str());

			return 1;
		}

		std::size_t code = 0;

		for (const GuardBuildSection& s : build.Sections())
		{
			code += s.Bytes.size();
		}

		std::printf("%s  %s (%zu bytes de codigo)\n", Hex(build.Hash(), GuardProto::HashSize).c_str(), out.c_str(), code);

		return 0;
	}

	std::signal(SIGPIPE, SIG_IGN);

	Guard guard;

	if (!guard.Start())
	{
		return 1;
	}

	guard.Run();

	return 0;
}
