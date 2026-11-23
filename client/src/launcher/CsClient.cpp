#include "CsClient.h"

#include "net/crypto/SecureRandom.h"
#include "update/Announce.h"

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
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Launcher {

namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kNoSocket = INVALID_SOCKET;
void CloseSocket(Socket s) { closesocket(s); }
bool InProgress() { const int e = WSAGetLastError(); return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void SetNonBlocking(Socket s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
struct WinsockInit
{
	WinsockInit() { WSADATA d; WSAStartup(MAKEWORD(2, 2), &d); }
	~WinsockInit() { WSACleanup(); }
};
#else
using Socket = int;
constexpr Socket kNoSocket = -1;
void CloseSocket(Socket s) { ::close(s); }
bool InProgress() { return errno == EINPROGRESS; }
void SetNonBlocking(Socket s) { ::fcntl(s, F_SETFL, ::fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

constexpr std::uint8_t kSub = 0xF4;
constexpr std::uint8_t kRequest = 0x14;
constexpr std::uint8_t kReply = 0x15;
constexpr std::size_t kReplyHeader = 16;   // PMSG_LAUNCHER_CONFIG_SEND

using Clock = std::chrono::steady_clock;

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

std::uint32_t ReadLe32(const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
	       static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

// Espera a que el socket se pueda leer (o escribir) hasta el plazo.  1: listo,
// 0: venció el plazo o se canceló, -1: error.
int WaitFor(Socket s, bool write, Clock::time_point deadline, const std::atomic<bool>* cancel)
{
	for (;;)
	{
		if (cancel && cancel->load()) return 0;
		const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
		if (left <= 0) return 0;
		fd_set set;
		FD_ZERO(&set);
		FD_SET(s, &set);
		timeval tv{};
		const long slice = left > 200 ? 200 : static_cast<long>(left);   // revisa cancel cada tanto
		tv.tv_sec = 0;
		tv.tv_usec = slice * 1000;
		const int n = ::select(static_cast<int>(s) + 1, write ? nullptr : &set, write ? &set : nullptr, nullptr, &tv);
		if (n < 0) return -1;
		if (n > 0) return 1;
	}
}

bool SendAll(Socket s, const std::uint8_t* data, std::size_t size, Clock::time_point deadline,
             const std::atomic<bool>* cancel)
{
	while (size > 0)
	{
		if (WaitFor(s, true, deadline, cancel) != 1) return false;
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

Socket Connect(const std::string& host, std::uint16_t port, Clock::time_point deadline,
               const std::atomic<bool>* cancel, std::string* error)
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* list = nullptr;
	if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list) != 0 || !list)
	{
		Fail(error, "no se encuentra " + host);
		return kNoSocket;
	}
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
		if (WaitFor(s, true, deadline, cancel) == 1)
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
	if (result == kNoSocket) Fail(error, "no responde " + host + ":" + std::to_string(port));
	return result;
}

} // namespace

bool SocketCsFetcher::Fetch(const std::string& host, std::uint16_t port, const SecureChannel::TrustFn& trust,
                            std::vector<std::uint8_t>& blob, std::uint8_t identity[SecureChannel::KeySize],
                            std::string* error)
{
#ifdef _WIN32
	static WinsockInit winsock;
#endif
	const std::string where = host + ":" + std::to_string(port);
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(m_TimeoutMs);
	Socket s = Connect(host, port, deadline, m_Cancel, error);
	if (s == kNoSocket) return false;

	SecureChannel channel;
	std::uint8_t random[SecureChannel::KeySize];
	if (!SecureRandomBytes(random, sizeof(random)))
	{
		CloseSocket(s);
		return Fail(error, "no hay números aleatorios");
	}
	bool trusted = false;
	channel.BeginClient(random, [&](const std::uint8_t* id, const std::uint8_t* prev, const std::uint8_t* sig) {
		trusted = trust(id, prev, sig);
		return trusted;
	});
	std::memset(random, 0, sizeof(random));

	std::vector<std::uint8_t> plain;
	std::uint8_t buf[4096];
	bool asked = false;
	std::uint32_t total = 0;
	blob.clear();
	auto request = [&](std::uint32_t offset) {
		const std::uint8_t pkt[8] = { 0xC1, 8, kSub, kRequest, static_cast<std::uint8_t>(offset),
		                              static_cast<std::uint8_t>(offset >> 8), static_cast<std::uint8_t>(offset >> 16),
		                              static_cast<std::uint8_t>(offset >> 24) };
		std::uint8_t sealed[sizeof(pkt) + SecureChannel::Overhead];
		const std::size_t n = channel.Seal(pkt, sizeof(pkt), sealed);
		return n != 0 && SendAll(s, sealed, n, deadline, m_Cancel);
	};
	auto done = [&](bool ok, const std::string& why) {
		CloseSocket(s);
		if (!ok) Fail(error, why);
		return ok;
	};

	for (;;)
	{
		const int w = WaitFor(s, false, deadline, m_Cancel);
		if (w == 0) return done(false, (m_Cancel && m_Cancel->load()) ? "cancelado" : where + " no contestó a tiempo");
		if (w < 0) return done(false, "se cortó la conexión con " + where);
		const int n = ::recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
		if (n == 0) return done(false, where + " cerró la conexión");
		if (n < 0)
		{
			if (InProgress()) continue;
			return done(false, "se cortó la conexión con " + where);
		}
		if (!channel.Receive(buf, static_cast<std::size_t>(n), plain))
		{
			if (channel.GetFailure() == SecureChannel::Failure::Untrusted || (!trusted && channel.GetFailure() == SecureChannel::Failure::BadSignature))
				return done(false, where + " no presentó la identidad del server");
			if (channel.GetFailure() == SecureChannel::Failure::NotThisProtocol)
				return done(false, where + " no usa el canal cifrado");
			return done(false, "el canal con " + where + " falló");
		}
		std::uint8_t reply[SecureChannel::ClientHelloSize];
		if (const std::size_t r = channel.TakeReply(reply))
			if (!SendAll(s, reply, r, deadline, m_Cancel)) return done(false, "se cortó la conexión con " + where);
		if (!channel.IsReady()) continue;
		if (!asked)
		{
			asked = true;
			std::memcpy(identity, channel.PeerIdentity(), SecureChannel::KeySize);
			if (!request(0)) return done(false, "se cortó la conexión con " + where);
		}

		// Paquetes completos (C1: largo en 1 byte; C2: en 2, big-endian).
		std::size_t pos = 0;
		while (pos + 3 <= plain.size())
		{
			const std::uint8_t type = plain[pos];
			std::size_t size = 0;
			if (type == 0xC1) size = plain[pos + 1];
			else if (type == 0xC2) size = static_cast<std::size_t>(plain[pos + 1]) << 8 | plain[pos + 2];
			else return done(false, "respuesta inválida de " + where);
			if (size < 3) return done(false, "respuesta inválida de " + where);
			if (pos + size > plain.size()) break;
			const std::uint8_t* p = plain.data() + pos;
			pos += size;
			if (type != 0xC2 || size < kReplyHeader || p[3] != kSub || p[4] != kReply) continue;   // la bienvenida, etc.
			const std::uint8_t result = p[5];
			const std::uint32_t fileSize = ReadLe32(p + 6), offset = ReadLe32(p + 10);
			const std::size_t length = static_cast<std::size_t>(p[14]) | static_cast<std::size_t>(p[15]) << 8;
			if (result == 4) return done(false, where + " no publica launcher.json");
			if (result != 0) return done(false, where + " rechazó el pedido (" + std::to_string(result) + ")");
			if (fileSize <= 64 || fileSize > 64 + Update::kAnnounceMaxSize || kReplyHeader + length > size)
				return done(false, "respuesta inválida de " + where);
			if (blob.empty()) total = fileSize;
			if (fileSize != total || offset != blob.size() || offset + length > total || length == 0)
				return done(false, "launcher.json cambió mientras se bajaba de " + where);
			blob.insert(blob.end(), p + kReplyHeader, p + kReplyHeader + length);
			if (blob.size() == total) return done(true, "");
			if (!request(static_cast<std::uint32_t>(blob.size()))) return done(false, "se cortó la conexión con " + where);
		}
		plain.erase(plain.begin(), plain.begin() + static_cast<std::ptrdiff_t>(pos));
	}
}

} // namespace Launcher
