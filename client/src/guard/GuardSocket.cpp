// Ver GuardSocket.h.  Sin stdafx.h: sólo sockets y el canal.

#include "guard/GuardSocket.h"

#include "net/crypto/SecureRandom.h"

#include <chrono>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Guard {

namespace {

using Clock = std::chrono::steady_clock;

#ifdef _WIN32
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool InProgress() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void SetNonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
void StartWinsock() { static bool started = false; if (!started) { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); started = true; } }
#else
using Socket = int;
const Socket kNoSocket = -1;
void CloseSocket(Socket s) { ::close(s); }
bool InProgress() { return errno == EINPROGRESS || errno == EAGAIN || errno == EWOULDBLOCK; }
// FD_CLOEXEC: el juego que lanza el guardián no tiene que heredarlo.
void SetNonBlocking(Socket s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); ::fcntl(s, F_SETFD, FD_CLOEXEC); }
void StartWinsock() {}
#endif

Socket ToSocket(std::intptr_t s) { return static_cast<Socket>(s); }

// 1: listo, 0: venció el plazo, -1: error.
int WaitFor(Socket s, bool write, Clock::time_point deadline)
{
	const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
	if (left < 0) return 0;   // 0: se mira sin esperar
	fd_set set;
	FD_ZERO(&set);
	FD_SET(s, &set);
	timeval tv{};
	tv.tv_sec = static_cast<long>(left / 1000);
	tv.tv_usec = static_cast<long>((left % 1000) * 1000);
	const int n = ::select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
	return n < 0 ? -1 : (n > 0 ? 1 : 0);
}

bool SendAll(Socket s, const std::uint8_t* data, std::size_t size, Clock::time_point deadline)
{
	while (size > 0)
	{
		if (WaitFor(s, true, deadline) != 1) return false;
		const int n = ::send(s, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
		if (n <= 0)
		{
			if (InProgress()) continue;
			return false;
		}
		data += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

} // namespace

bool SecureSocket::Open(const std::string& host, std::uint16_t port, int timeoutMs, const SecureChannel::TrustFn& trust)
{
	this->Close();
	StartWinsock();

	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);

	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* list = nullptr;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list) return false;

	Socket result = kNoSocket;
	for (addrinfo* a = list; a && result == kNoSocket; a = a->ai_next)
	{
		Socket s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
		if (s == kNoSocket) continue;
		SetNonBlocking(s);
		if (::connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) != 0 && !InProgress())
		{
			CloseSocket(s);
			continue;
		}
		if (WaitFor(s, true, deadline) == 1)
		{
			int soError = 0;
#ifdef _WIN32
			int len = sizeof(soError);
			::getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
#else
			socklen_t len = sizeof(soError);
			::getsockopt(s, SOL_SOCKET, SO_ERROR, &soError, &len);
#endif
			if (soError == 0)
			{
				result = s;
				break;
			}
		}
		CloseSocket(s);
	}
	::freeaddrinfo(list);
	if (result == kNoSocket) return false;

	const int on = 1;
	::setsockopt(result, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof(on));

	this->m_Socket = static_cast<std::intptr_t>(result);

	std::uint8_t random[SecureChannel::KeySize];
	if (!SecureRandomBytes(random, sizeof(random)))
	{
		this->Close();
		return false;
	}
	this->m_Channel.BeginClient(random, trust);
	std::memset(random, 0, sizeof(random));

	// El saludo: hasta que el canal quede listo, sin texto claro de por medio.
	std::vector<std::uint8_t> plain;
	while (!this->m_Channel.IsReady())
	{
		const int remaining = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
		if (remaining <= 0 || !this->Receive(plain, remaining))
		{
			this->Close();
			return false;
		}
	}
	return true;
}

bool SecureSocket::Send(const void* data, std::size_t size, int timeoutMs)
{
	if (!this->IsOpen()) return false;
	std::vector<std::uint8_t> sealed(size + SecureChannel::Overhead);
	const std::size_t n = this->m_Channel.Seal(static_cast<const std::uint8_t*>(data), size, sealed.data());
	if (n == 0 || !SendAll(ToSocket(this->m_Socket), sealed.data(), n, Clock::now() + std::chrono::milliseconds(timeoutMs)))
	{
		this->Close();
		return false;
	}
	return true;
}

bool SecureSocket::Receive(std::vector<std::uint8_t>& plain, int timeoutMs)
{
	if (!this->IsOpen()) return false;
	const Socket s = ToSocket(this->m_Socket);
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
	const std::size_t before = plain.size();
	std::uint8_t buffer[4096];

	for (;;)
	{
		const int w = WaitFor(s, false, deadline);
		if (w == 0) return false;
		if (w < 0)
		{
			this->Close();
			return false;
		}
		const int n = ::recv(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
		if (n == 0)
		{
			this->Close();
			return false;
		}
		if (n < 0)
		{
			if (InProgress()) continue;
			this->Close();
			return false;
		}
		const bool wasReady = this->m_Channel.IsReady();
		if (!this->m_Channel.Receive(buffer, static_cast<std::size_t>(n), plain))
		{
			this->Close();
			return false;
		}
		std::uint8_t reply[SecureChannel::ClientHelloSize];
		if (const std::size_t r = this->m_Channel.TakeReply(reply))
		{
			if (!SendAll(s, reply, r, deadline))
			{
				this->Close();
				return false;
			}
		}
		// El saludo terminó en esta lectura, o llegó texto claro.
		if ((!wasReady && this->m_Channel.IsReady()) || plain.size() > before) return true;
	}
}

void SecureSocket::Close()
{
	if (this->m_Socket != kNone)
	{
		CloseSocket(ToSocket(this->m_Socket));
		this->m_Socket = kNone;
	}
	this->m_Channel.Reset();
}

} // namespace Guard
