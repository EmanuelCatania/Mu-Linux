#include "stdafx.h"
#include "GuardLink.h"

#if defined(MU_GUARD) && !defined(_WIN32)

#include "GuardConfig.h"
#include "GuardProtocol.h"
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

			GuardProto::Append(ping, GuardProto::Ping, nullptr, 0);

			std::uint8_t type = 0;

			std::vector<std::uint8_t> payload;

			alive = SendAll(fd, ping) && ReceiveMessage(fd, pending, type, payload) && type == GuardProto::Pong;

			if (!alive)
			{
				::close(fd);

				fd = -1;
			}
		}

		if (alive)
		{
			missed = 0;

			ok++;

			if (this->m_State == Starting || (this->m_State == Down && ok >= kOkForUp))
			{
				LogAdd(LOG_BLUE, "[MU-Guard] mu-guard ARRIBA");

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

GuardLogin CGuardLink::CheckLogin(const char* ip, const std::uint8_t* ticket, const std::uint8_t* mac,
                                  const std::uint8_t* transcript)
{
	const GuardSettings settings = ReadGuardSettings(this->m_Data);

	if (!settings.Enabled)
	{
		return GuardLogin::Accept;
	}

	// Caído (lo decide este GameServer, no el cliente): se entra sin validar.
	if (this->m_State == Down)
	{
		LogAdd(LOG_RED, "[MU-Guard] [%s] Login sin validar: mu-guard caido", ip);

		return GuardLogin::Unvalidated;
	}

	int result = -1;

	if (ticket != nullptr && mac != nullptr && transcript != nullptr)
	{
		result = this->Verify(ip, ticket, mac, transcript);
	}

	if (result == GuardProto::Valid)
	{
		return GuardLogin::Accept;
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
