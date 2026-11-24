// mu-guard: la parte de MU-Guard que corre en el server (Fase 5).  Diseño en
// doc-port/diseno-seguridad-fase5.md; mensajes en Common/GuardProtocol.h.
//
// Dos puertas:
//   - TCP en el puerto público (Guard.toml Port, 44406): el guardián del
//     cliente, por el mismo canal cifrado que el juego (SecureChannel) y con la
//     misma identidad del server (Data/Keys/ServerIdentity.key, la genera el
//     GameServer; acá sólo se lee).  Saluda y pide tickets.
//   - Socket Unix local (Guard.toml Socket): el GameServer.  Latido y
//     verificación de los tickets que llegan en el login.
//
// Un solo hilo con poll(): son pocas conexiones y cada mensaje se atiende en
// microsegundos.  Sólo Linux, como mu-trace.

#include "stdafx.h"
#include "GuardConfig.h"
#include "GuardProtocol.h"
#include "SecureChannel.h"
#include "SecureRandom.h"
#include "ServerIdentity.h"

extern "C"
{
#include "monocypher/monocypher.h"
}

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <memory>
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
		Clock::time_point expires;
	};

	struct Connection
	{
		int fd = -1;
		bool local = false;           // GameServer por el socket Unix
		std::string ip;
		Clock::time_point opened;
		bool greeted = false;         // el guardián mandó un Hello válido
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

		void Tick();

		int ConnectionsFrom(const std::string& ip) const;

		std::string m_Data;

		GuardSettings m_Settings;

		int m_Tcp = -1;

		int m_Unix = -1;

		std::vector<std::unique_ptr<Connection>> m_Connections;

		std::map<std::string, Ticket> m_Tickets;   // id en binario -> ticket

		std::map<std::string, std::vector<Clock::time_point>> m_Issued;   // IP -> tickets del último minuto

		Clock::time_point m_NextTick;
	};

	bool Guard::Start()
	{
		this->m_Data = DataPath();

		this->m_Settings = ReadGuardSettings(this->m_Data);

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

		this->m_NextTick = Clock::now();

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
				this->Reply(c, Pong, nullptr, 0);
			}
			else if (type == Verify)
			{
				this->HandleVerify(c, payload);
			}
			else
			{
				c.closing = true;
			}

			return;
		}

		if (type == Hello)
		{
			std::uint8_t result = Ok;

			if (payload.size() != HelloSize || (payload[0] | (payload[1] << 8)) != Version)
			{
				result = BadVersion;
			}

			// El hash del main en disco (payload + 2) todavía no se compara: los
			// builds aceptados llegan con el desafío de memoria (paso 4).
			c.greeted = result == Ok;

			this->Reply(c, HelloResult, &result, 1);

			if (!c.greeted)
			{
				LogAdd(LOG_RED, "[MU-Guard] [%s] Guardian de otra version: rechazado", c.ip.c_str());
			}

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

		if (it == this->m_Tickets.end())
		{
			// Un id ya usado tampoco está: se borra al presentarlo.
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
			else if (ticket.ip != ip)
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
			}

			crypto_wipe(expected, sizeof(expected));
		}

		if (reply[4] != Valid)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Ticket rechazado en el login (motivo %d)", ip, (int)reply[4]);
		}

		this->Reply(c, VerifyResult, reply, sizeof(reply));
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

		// La clave la rota el GameServer: se relee si cambió.
		if (gServerIdentity.Reload((this->m_Data + "Keys").c_str()))
		{
			LogAdd(LOG_BLUE, "[MU-Guard] Clave del server cargada");
		}

		this->m_Settings = ReadGuardSettings(this->m_Data);
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

int main()
{
	std::signal(SIGPIPE, SIG_IGN);

	Guard guard;

	if (!guard.Start())
	{
		return 1;
	}

	guard.Run();

	return 0;
}
