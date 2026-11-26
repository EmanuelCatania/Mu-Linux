#include "stdafx.h"
#include "GuardLink.h"

#if defined(MU_GUARD) && !defined(_WIN32)

#include "GuardConfig.h"
#include "GuardProtocol.h"
#include "SecureLink.h"
#include "User.h"
#include "Util.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

CGuardLink gGuardLink;

namespace
{
	using Clock = std::chrono::steady_clock;

	constexpr int kMissedForDown = 5;
	constexpr int kOkForUp = 3;
	constexpr int kStartupSeconds = 10;
	constexpr int kVerifyTimeoutMs = 300;
	constexpr std::size_t kMaxSessions = 20000;
	constexpr int kStatusSeconds = 5;

	// Socket local conectado y con plazo de lectura/escritura; -1 si no se pudo.
	int ConnectLocal(const std::string& path, int timeoutMs)
	{
		sockaddr_un address {};
		address.sun_family = AF_UNIX;

		if (path.size() >= sizeof(address.sun_path))
		{
			return -1;
		}

		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);

		const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);

		if (fd < 0)
		{
			return -1;
		}

		timeval tv {};
		tv.tv_sec = timeoutMs / 1000;
		tv.tv_usec = (timeoutMs % 1000) * 1000;

		::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		if (::connect(fd, (sockaddr*)&address, sizeof(address)) != 0)
		{
			::close(fd);

			return -1;
		}

		return fd;
	}

	bool SendAll(int fd, const std::vector<std::uint8_t>& data)
	{
		std::size_t sent = 0;

		while (sent < data.size())
		{
			const ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);

			if (n <= 0)
			{
				return false;
			}

			sent += (std::size_t)n;
		}

		return true;
	}

	// Espera un mensaje entero hasta el plazo del socket.
	bool ReceiveMessage(int fd, std::vector<std::uint8_t>& pending, std::uint8_t& type, std::vector<std::uint8_t>& payload)
	{
		for (;;)
		{
			const int got = GuardProto::Pop(pending, type, payload);

			if (got != 0)
			{
				return got > 0;
			}

			std::uint8_t buffer[512];

			const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);

			if (n <= 0)
			{
				return false;
			}

			pending.insert(pending.end(), buffer, buffer + n);
		}
	}
}

void CGuardLink::Start(const std::string& dataPath)
{
	this->m_Data = dataPath;

	if (!this->m_Data.empty() && this->m_Data.back() != '/')
	{
		this->m_Data += '/';
	}

	this->m_Thread = std::thread(&CGuardLink::Run, this);

	this->m_Thread.detach();
}

void CGuardLink::Run()
{
	int fd = -1;

	int missed = 0;

	int ok = 0;

	Clock::time_point started = Clock::now();

	std::vector<std::uint8_t> pending;

	for (;; std::this_thread::sleep_for(std::chrono::seconds(1)))
	{
		const GuardSettings settings = ReadGuardSettings(this->m_Data);

		if (!settings.Enabled)
		{
			if (this->m_State != Off)
			{
				LogAdd(LOG_BLUE, "[MU-Guard] Apagado (Data/Guard/Guard.toml)");
			}

			this->m_State = Off;

			if (fd >= 0)
			{
				::close(fd);

				fd = -1;
			}

			continue;
		}

		if (this->m_State == Off)
		{
			this->m_State = Starting;

			started = Clock::now();

			missed = 0;

			ok = 0;
		}

		if (fd < 0)
		{
			fd = ConnectLocal(settings.Socket, 900);

			pending.clear();
		}

		bool alive = false;

		if (fd >= 0)
		{
			std::vector<std::uint8_t> ping;

			{
				// Lo que se juntó de Identify sale antes del latido, en el mismo envío.
				std::lock_guard<std::mutex> lock(this->m_Lock);

				for (const std::vector<std::uint8_t>& body : this->m_Identities)
				{
					GuardProto::Append(ping, GuardProto::Identify, body.data(), body.size());
				}

				this->m_Identities.clear();
			}

			GuardProto::Append(ping, GuardProto::Ping, nullptr, 0);

			std::uint8_t type = 0;

			std::vector<std::uint8_t> payload;

			alive = SendAll(fd, ping) && ReceiveMessage(fd, pending, type, payload) && type == GuardProto::Pong;

			if (!alive)
			{
				::close(fd);

				fd = -1;
			}
			else if (payload.size() >= 2)
			{
				const std::size_t count = payload[0] | (payload[1] << 8);

				if (payload.size() == 2 + count * GuardProto::TicketIdSize && count > 0)
				{
					std::lock_guard<std::mutex> lock(this->m_Lock);

					for (std::size_t i = 0; i < count; i++)
					{
						const char* id = (const char*)payload.data() + 2 + i * GuardProto::TicketIdSize;

						this->m_Kicks.emplace_back(id, GuardProto::TicketIdSize);
					}
				}
			}
		}

		if (alive)
		{
			missed = 0;

			ok++;

			if (this->m_State == Starting || (this->m_State == Down && ok >= kOkForUp))
			{
				LogAdd(LOG_BLUE, "[MU-Guard] mu-guard ARRIBA");

				if (this->m_State == Down)
				{
					this->m_Epoch++;
				}

				this->m_State = Up;
			}
		}
		else
		{
			ok = 0;

			missed++;

			const bool startupOver = Clock::now() - started >= std::chrono::seconds(kStartupSeconds);

			if ((this->m_State == Up && missed >= kMissedForDown) || (this->m_State == Starting && startupOver))
			{
				LogAdd(LOG_RED, "[MU-Guard] mu-guard CAIDO: se deja entrar sin validar hasta que vuelva");

				this->m_State = Down;
			}
		}
	}
}

int CGuardLink::Verify(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac, const std::uint8_t* transcript)
{
	using namespace GuardProto;

	const GuardSettings settings = ReadGuardSettings(this->m_Data);

	const int fd = ConnectLocal(settings.Socket, kVerifyTimeoutMs);

	if (fd < 0)
	{
		return -1;
	}

	const std::uint32_t request = ++this->m_Request;

	std::uint8_t body[VerifySize] = {};

	PutLe32(body, request);

	std::memcpy(body + 4, ticket, TicketIdSize);

	std::memcpy(body + 4 + TicketIdSize, mac, MacSize);

	std::memcpy(body + 4 + TicketIdSize + MacSize, transcript, TranscriptSize);

	std::strncpy((char*)body + 4 + TicketIdSize + MacSize + TranscriptSize, ip, IpSize - 1);

	std::vector<std::uint8_t> message;

	Append(message, GuardProto::Verify, body, sizeof(body));

	std::vector<std::uint8_t> pending;

	std::uint8_t type = 0;

	std::vector<std::uint8_t> payload;

	int result = -1;

	if (SendAll(fd, message) && ReceiveMessage(fd, pending, type, payload) && type == VerifyResult &&
		payload.size() == VerifyResultSize && GetLe32(payload.data()) == request)
	{
		result = payload[4];
	}

	::close(fd);

	return result;
}

namespace
{
	bool SameConnection(int index, std::uint64_t socket)
	{
		if (!OBJECT_USER_RANGE(index))
		{
			return false;
		}

		const LPOBJ lpObj = &gObj[index];

		return lpObj->Connected != OBJECT_OFFLINE && (std::uint64_t)lpObj->Socket == socket;
	}
}

void CGuardLink::Bind(const std::uint8_t* ticket, int aIndex)
{
	std::lock_guard<std::mutex> lock(this->m_Lock);

	// Un segundo login por la misma conexión reemplaza al anterior.
	auto bound = this->m_Bound.find(aIndex);

	if (bound != this->m_Bound.end())
	{
		this->m_Sessions.erase(bound->second);

		this->m_Bound.erase(bound);
	}

	if (this->m_Sessions.size() >= kMaxSessions)
	{
		// Se van las de conexiones que ya no son las que entraron (no debería
		// haber: gObjDel las saca).
		for (auto it = this->m_Sessions.begin(); it != this->m_Sessions.end();)
		{
			if (SameConnection(it->second.Index, it->second.Socket))
			{
				++it;

				continue;
			}

			this->m_Bound.erase(it->second.Index);

			it = this->m_Sessions.erase(it);
		}

		if (this->m_Sessions.size() >= kMaxSessions)
		{
			return;
		}
	}

	const std::string id((const char*)ticket, GuardProto::TicketIdSize);

	this->m_Sessions[id] = { aIndex, (std::uint64_t)gObj[aIndex].Socket };

	this->m_Bound[aIndex] = id;
}

void CGuardLink::Identify(int aIndex)
{
	if (!OBJECT_USER_RANGE(aIndex))
	{
		return;
	}

	const LPOBJ lpObj = &gObj[aIndex];

	std::lock_guard<std::mutex> lock(this->m_Lock);

	auto bound = this->m_Bound.find(aIndex);

	if (bound == this->m_Bound.end() || this->m_Identities.size() >= 4096)
	{
		return;
	}

	std::vector<std::uint8_t> body(GuardProto::IdentifySize, 0);

	std::memcpy(body.data(), bound->second.data(), GuardProto::TicketIdSize);

	std::strncpy((char*)body.data() + GuardProto::TicketIdSize, lpObj->Account, GuardProto::AccountSize);

	if (lpObj->Connected == OBJECT_ONLINE)
	{
		std::strncpy((char*)body.data() + GuardProto::TicketIdSize + GuardProto::AccountSize, lpObj->Name, GuardProto::AccountSize);
	}

	this->m_Identities.push_back(std::move(body));
}

void CGuardLink::Unbind(int aIndex)
{
	this->m_Pending.erase(aIndex);

	std::lock_guard<std::mutex> lock(this->m_Lock);

	auto bound = this->m_Bound.find(aIndex);

	if (bound == this->m_Bound.end())
	{
		return;
	}

	this->m_Sessions.erase(bound->second);

	this->m_Bound.erase(bound);
}

void CGuardLink::MarkPending(int aIndex)
{
	if (OBJECT_USER_RANGE(aIndex))
	{
		this->m_Pending[aIndex] = { (std::uint64_t)gObj[aIndex].Socket, false, {} };
	}
}

void CGuardLink::Revalidate(int aIndex, const std::uint8_t* ticket, const std::uint8_t* mac)
{
	auto it = this->m_Pending.find(aIndex);

	// Sólo la pide una sesión que entró sin validar, y una sola vez.
	if (it == this->m_Pending.end() || !it->second.Armed)
	{
		return;
	}

	this->m_Pending.erase(it);

	const LPOBJ lpObj = &gObj[aIndex];

	const std::uint8_t* transcript = gSecureLink.Transcript(aIndex);

	const int result = transcript != nullptr ? this->Verify(lpObj->IpAddr, ticket, mac, transcript) : -1;

	if (result == GuardProto::Valid)
	{
		this->Bind(ticket, aIndex);

		this->Identify(aIndex);

		LogAdd(LOG_BLUE, "[MU-Guard] [%s][%s] Sesion validada al volver mu-guard", lpObj->IpAddr, lpObj->Account);

		return;
	}

	LogAdd(LOG_RED, "[MU-Guard] [%s][%s] Desconectado: ticket invalido al volver mu-guard (%d)", lpObj->IpAddr, lpObj->Account, result);

	CloseClient(aIndex);
}

void CGuardLink::Tick()
{
	const GuardSettings settings = ReadGuardSettings(this->m_Data);

	if (!settings.Enabled)
	{
		this->m_Pending.clear();

		return;
	}

	const auto now = Clock::now();

	const bool up = this->m_State == Up;

	bool armed = false;

	for (auto it = this->m_Pending.begin(); it != this->m_Pending.end();)
	{
		if (!SameConnection(it->first, it->second.Socket) || !settings.Required)
		{
			it = this->m_Pending.erase(it);

			continue;
		}

		PendingSession& pending = it->second;

		if (!up)
		{
			// Se volvió a caer: el plazo arranca de nuevo cuando vuelva.
			pending.Armed = false;
		}
		else if (!pending.Armed)
		{
			pending.Armed = true;

			pending.Deadline = now + std::chrono::seconds(GuardProto::RevalidateSeconds);

			armed = true;
		}
		else if (now >= pending.Deadline)
		{
			const LPOBJ lpObj = &gObj[it->first];

			LogAdd(LOG_RED, "[MU-Guard] [%s][%s] Desconectado: no valido en %d s al volver mu-guard", lpObj->IpAddr, lpObj->Account, GuardProto::RevalidateSeconds);

			const int index = it->first;

			it = this->m_Pending.erase(it);

			CloseClient(index);

			continue;
		}

		++it;
	}

	// El estado va cada 5 s, y enseguida si alguien tiene que mandar un ticket.
	if (!armed && now - this->m_LastStatus < std::chrono::seconds(kStatusSeconds))
	{
		return;
	}

	this->m_LastStatus = now;

	const std::uint32_t epoch = this->m_Epoch;

	for (int n = OBJECT_START_USER; n < MAX_OBJECT; n++)
	{
		if (gObj[n].Connected < OBJECT_LOGGED || gSecureLink.Transcript(n) == nullptr)
		{
			continue;
		}

		std::uint8_t packet[GuardProto::GameStatusSize] = { 0xC1, (std::uint8_t)GuardProto::GameStatusSize, 0xF1, GuardProto::GameStatusSub };

		packet[4] = up ? GuardProto::GuardUp : GuardProto::GuardDown;

		auto pending = this->m_Pending.find(n);

		packet[5] = (pending != this->m_Pending.end() && pending->second.Armed) ? GuardProto::NeedTicket : 0;

		GuardProto::PutLe32(packet + 6, epoch);

		DataSend(n, packet, sizeof(packet));
	}
}

void CGuardLink::ProcessKicks()
{
	std::vector<std::string> kicks;

	{
		std::lock_guard<std::mutex> lock(this->m_Lock);

		if (this->m_Kicks.empty())
		{
			return;
		}

		kicks.swap(this->m_Kicks);
	}

	for (const std::string& ticket : kicks)
	{
		Session session;

		{
			std::lock_guard<std::mutex> lock(this->m_Lock);

			auto it = this->m_Sessions.find(ticket);

			if (it == this->m_Sessions.end())
			{
				continue;
			}

			session = it->second;

			this->m_Bound.erase(session.Index);

			this->m_Sessions.erase(it);
		}

		if (!SameConnection(session.Index, session.Socket))
		{
			continue;
		}

		const LPOBJ lpObj = &gObj[session.Index];

		LogAdd(LOG_RED, "[MU-Guard] [%s][%s] Desconectado por mu-guard", lpObj->IpAddr, lpObj->Account);

		CloseClient(session.Index);
	}
}

GuardLogin CGuardLink::CheckLogin(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac,
                                  const std::uint8_t* transcript, bool web)
{
	const GuardSettings settings = ReadGuardSettings(this->m_Data);

	if (!settings.Enabled)
	{
		return GuardLogin::Accept;
	}

	// Versión web: en el navegador no puede correr el guardián.  Sólo se mira
	// el ban de IP; el resto lo controla el server (velocidad, movimiento,
	// paquetes).  Con WebClients = false no se entra.
	if (web)
	{
		if (!settings.WebClients)
		{
			LogAdd(LOG_RED, "[MU-Guard] [%s] Login rechazado: cliente web (WebClients = false)", ip);

			return GuardLogin::Reject;
		}

		if (this->m_State != Down)
		{
			const std::uint8_t none[GuardProto::TicketIdSize + GuardProto::MacSize + GuardProto::TranscriptSize] = {};

			if (this->Verify(ip, none, none + GuardProto::TicketIdSize, none + GuardProto::TicketIdSize + GuardProto::MacSize) == GuardProto::BannedIp)
			{
				return GuardLogin::Banned;
			}
		}

		LogAdd(LOG_BLUE, "[MU-Guard] [%s] Login web: sin guardián, controles del server", ip);

		return GuardLogin::Unvalidated;
	}

	// Caído (lo decide este GameServer, no el cliente): se entra sin validar.
	// Si se exige el ticket, la sesión valida cuando vuelve (Tick).
	if (this->m_State == Down)
	{
		LogAdd(LOG_RED, "[MU-Guard] [%s] Login sin validar: mu-guard caido", ip);

		return settings.Required ? GuardLogin::Pending : GuardLogin::Unvalidated;
	}

	int result = -1;

	if (ticket != nullptr && mac != nullptr && transcript != nullptr)
	{
		result = this->Verify(ip, ticket, mac, transcript);
	}
	else
	{
		// Sin ticket: igual se pregunta por la IP (id en ceros).
		const std::uint8_t none[GuardProto::TicketIdSize + GuardProto::MacSize + GuardProto::TranscriptSize] = {};

		result = this->Verify(ip, none, none + GuardProto::TicketIdSize, none + GuardProto::TicketIdSize + GuardProto::MacSize);
	}

	if (result == GuardProto::Valid)
	{
		return GuardLogin::Accept;
	}

	if (result == GuardProto::BannedIp)
	{
		return GuardLogin::Banned;
	}

	if (!settings.Required)
	{
		LogAdd(LOG_BLUE, "[MU-Guard] [%s] Login sin guardian valido (%s); no se exige", ip,
			ticket == nullptr ? "sin ticket" : "ticket invalido");

		return GuardLogin::Unvalidated;
	}

	LogAdd(LOG_RED, "[MU-Guard] [%s] Login rechazado: %s", ip, ticket == nullptr ? "sin ticket" : (result < 0 ? "mu-guard no contesto" : "ticket invalido"));

	return GuardLogin::Reject;
}

#endif
