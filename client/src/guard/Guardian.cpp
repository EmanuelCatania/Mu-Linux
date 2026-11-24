// Ver Guardian.h.

#include "stdafx.h"
#include "guard/Guardian.h"

#include "guard/GuardProtocol.h"
#include "guard/GuardSocket.h"

#include "assets/data/ContentCatalog.h"
#include "net/ServerTrust.h"
#include "platform/UserPath.h"

extern "C" {
#include "net/crypto/monocypher.h"
}

extern "C" void DbgLogPublic(const char* msg);

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace Guardian {

namespace {

using Clock = std::chrono::steady_clock;

// Variables con las que el guardián le pasa el pipe al juego.  El juego las
// borra al leerlas: el launcher que pueda abrir después no las tiene que heredar.
#ifdef _WIN32
const char kPipeVar[] = "MU_GUARD_PIPE";   // "<lectura>,<escritura>" (handles)
#else
const char kPipeVar[] = "MU_GUARD_FD";     // descriptor de un socketpair
#endif
const char kParentVar[] = "MU_GUARD_PARENT";

// Pedido del juego: 'T' | número de pedido (1).  Respuesta del guardián:
// número de pedido (1) | resultado (1) | id (16) | clave (32).  El número deja
// descartar la respuesta tardía de un pedido que el juego ya dio por perdido.
const std::uint8_t kAskTicket = 'T';
constexpr std::size_t kTicketReply = GuardProto::TicketSize;

void Log(const char* format, ...)
{
	char line[512];
	va_list args;
	va_start(args, format);
	std::vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	DbgLogPublic(line);
}

bool Trust(const std::uint8_t* id, const std::uint8_t* previous, const std::uint8_t* signature)
{
	return ServerTrust::Check(id, previous, signature) != ServerTrust::Result::Mismatch;
}

// ── Mensajes con mu-guard ────────────────────────────────────────────────────

class GuardConnection
{
public:
	void Configure(const std::string& host, std::uint16_t port, const std::uint8_t mainHash[GuardProto::HashSize])
	{
		this->m_Host = host;
		this->m_Port = port;
		std::memcpy(this->m_MainHash, mainHash, sizeof(this->m_MainHash));
	}

	// Conecta y saluda si hace falta.
	bool Ensure()
	{
		if (this->m_Socket.IsOpen()) return true;
		this->m_Pending.clear();
		if (!this->m_Socket.Open(this->m_Host, this->m_Port, 3000, Trust)) return false;

		std::uint8_t hello[GuardProto::HelloSize];
		hello[0] = static_cast<std::uint8_t>(GuardProto::Version & 0xFF);
		hello[1] = static_cast<std::uint8_t>(GuardProto::Version >> 8);
		std::memcpy(hello + 2, this->m_MainHash, GuardProto::HashSize);

		std::vector<std::uint8_t> reply;
		if (!this->Exchange(GuardProto::Hello, hello, sizeof(hello), GuardProto::HelloResult, reply) || reply.size() != 1 || reply[0] != GuardProto::Ok)
		{
			Log("MU-Guard: mu-guard no acepto el saludo");
			this->m_Socket.Close();
			return false;
		}
		return true;
	}

	bool Ticket(std::uint8_t out[kTicketReply])
	{
		std::vector<std::uint8_t> reply;
		// Si la conexión se había cortado, un reintento con una nueva.
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!this->Ensure()) return false;
			if (this->Exchange(GuardProto::TicketRequest, nullptr, 0, GuardProto::Ticket, reply) && reply.size() == kTicketReply)
			{
				std::memcpy(out, reply.data(), kTicketReply);
				crypto_wipe(reply.data(), reply.size());
				return out[0] == GuardProto::Ok;
			}
			this->m_Socket.Close();
		}
		return false;
	}

	bool Open() const { return this->m_Socket.IsOpen(); }

private:
	bool Exchange(std::uint8_t type, const void* data, std::size_t size, std::uint8_t expect, std::vector<std::uint8_t>& payload)
	{
		std::vector<std::uint8_t> message;
		GuardProto::Append(message, type, data, size);
		if (!this->m_Socket.Send(message.data(), message.size(), 3000)) return false;

		const Clock::time_point deadline = Clock::now() + std::chrono::seconds(3);
		for (;;)
		{
			std::uint8_t got = 0;
			const int popped = GuardProto::Pop(this->m_Pending, got, payload);
			if (popped < 0) return false;
			if (popped > 0)
			{
				if (got == expect) return true;
				continue;   // otro mensaje: no es la respuesta
			}
			const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
			if (left <= 0 || !this->m_Socket.Receive(this->m_Pending, left)) return false;
		}
	}

	std::string m_Host;
	std::uint16_t m_Port = 0;
	std::uint8_t m_MainHash[GuardProto::HashSize] = {};
	Guard::SecureSocket m_Socket;
	std::vector<std::uint8_t> m_Pending;
};

// Pregunta al ConnectServer si hay MU-Guard (F4:16).  Devuelve el puerto de
// mu-guard, o 0 si no hay (o no se pudo saber).
std::uint16_t AskConnectServer(const std::string& host, std::uint16_t port)
{
	Guard::SecureSocket cs;
	if (!cs.Open(host, port, 4000, Trust))
	{
		Log("MU-Guard: el ConnectServer %s:%u no contesto por el canal cifrado; sigue sin guardian", host.c_str(), port);
		return 0;
	}
	const std::uint8_t request[4] = { 0xC1, 4, 0xF4, 0x16 };
	if (!cs.Send(request, sizeof(request), 2000)) return 0;

	std::vector<std::uint8_t> plain;
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(4);
	std::size_t pos = 0;
	for (;;)
	{
		// Paquetes completos (C1: largo en 1 byte; C2: en 2, big-endian).
		while (pos + 3 <= plain.size())
		{
			const std::uint8_t* p = plain.data() + pos;
			std::size_t size = 0;
			if (p[0] == 0xC1) size = p[1];
			else if (p[0] == 0xC2) size = static_cast<std::size_t>(p[1]) << 8 | p[2];
			else return 0;
			if (size < 3) return 0;
			if (pos + size > plain.size()) break;
			pos += size;
			// C1:F4:17 (PMSG_CLIENT_POLICY_SEND): result, flags, puerto.
			if (p[0] == 0xC1 && size >= 6 && p[2] == 0xF4 && p[3] == 0x17)
			{
				if (p[4] != 0 || !(p[5] & 0x02) || size < 8) return 0;   // CONTENT_OK y CLIENT_POLICY_GUARD
				return static_cast<std::uint16_t>(p[6] | (p[7] << 8));
			}
		}
		const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
		if (left <= 0 || !cs.Receive(plain, left)) return 0;
	}
}

// BLAKE2b-256 del ejecutable en disco (va en el saludo; lo compara el paso 4).
bool HashFile(const std::string& path, std::uint8_t out[GuardProto::HashSize])
{
	FILE* file = std::fopen(path.c_str(), "rb");
	if (!file) return false;
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, GuardProto::HashSize);
	std::vector<std::uint8_t> buffer(1 << 16);
	std::size_t n;
	while ((n = std::fread(buffer.data(), 1, buffer.size(), file)) > 0) crypto_blake2b_update(&ctx, buffer.data(), n);
	std::fclose(file);
	crypto_blake2b_final(&ctx, out);
	return true;
}

std::string ExecutablePath()
{
#ifdef _WIN32
	wchar_t wide[MAX_PATH * 4];
	const DWORD n = GetModuleFileNameW(nullptr, wide, static_cast<DWORD>(sizeof(wide) / sizeof(wide[0])));
	if (n == 0) return std::string();
	char narrow[MAX_PATH * 8];
	const int len = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(n), narrow, sizeof(narrow) - 1, nullptr, nullptr);
	return std::string(narrow, len > 0 ? static_cast<std::size_t>(len) : 0);
#else
	char path[4096];
	const ssize_t n = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
	return n > 0 ? std::string(path, static_cast<std::size_t>(n)) : std::string();
#endif
}

// ── El pipe con el juego ─────────────────────────────────────────────────────

#ifdef _WIN32
using PipeEnd = HANDLE;

bool ReadExact(HANDLE h, void* data, std::size_t size)
{
	std::uint8_t* p = static_cast<std::uint8_t*>(data);
	while (size > 0)
	{
		DWORD got = 0;
		if (!ReadFile(h, p, static_cast<DWORD>(size), &got, nullptr) || got == 0) return false;
		p += got;
		size -= got;
	}
	return true;
}

bool WriteExact(HANDLE h, const void* data, std::size_t size)
{
	const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
	while (size > 0)
	{
		DWORD put = 0;
		if (!WriteFile(h, p, static_cast<DWORD>(size), &put, nullptr) || put == 0) return false;
		p += put;
		size -= put;
	}
	return true;
}
#else
bool ReadExact(int fd, void* data, std::size_t size)
{
	std::uint8_t* p = static_cast<std::uint8_t*>(data);
	while (size > 0)
	{
		const ssize_t got = ::read(fd, p, size);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) return false;
		p += got;
		size -= static_cast<std::size_t>(got);
	}
	return true;
}

bool WriteExact(int fd, const void* data, std::size_t size)
{
	const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
	while (size > 0)
	{
		const ssize_t put = ::send(fd, p, size, MSG_NOSIGNAL);
		if (put < 0 && errno == EINTR) continue;
		if (put <= 0) return false;
		p += put;
		size -= static_cast<std::size_t>(put);
	}
	return true;
}
#endif

// El guardián atiende los pedidos del juego hasta que el pipe se cierra.
#ifdef _WIN32
void Serve(HANDLE in, HANDLE out, GuardConnection& guard)
#else
void Serve(int in, int out, GuardConnection& guard)
#endif
{
	std::uint8_t ask[2] = {};
	while (ReadExact(in, ask, sizeof(ask)))
	{
		std::uint8_t reply[1 + kTicketReply] = {};
		reply[0] = ask[1];
		reply[1] = GuardProto::NotReady;
		if (ask[0] != kAskTicket || !guard.Ticket(reply + 1))
		{
			Log("MU-Guard: no se pudo obtener un ticket de mu-guard");
			std::memset(reply + 1, 0, kTicketReply);
			reply[1] = GuardProto::NotReady;
		}
		const bool sent = WriteExact(out, reply, sizeof(reply));
		crypto_wipe(reply, sizeof(reply));
		if (!sent) break;
	}
}

// ── Rol JUEGO ────────────────────────────────────────────────────────────────

std::mutex s_GameLock;
bool s_GameRole = false;
std::uint8_t s_Sequence = 0;
#ifdef _WIN32
HANDLE s_FromGuardian = nullptr;   // el juego lee
HANDLE s_ToGuardian = nullptr;     // el juego escribe
#else
int s_GuardianFd = -1;
#endif

#ifdef _WIN32
// Espera hasta `size` bytes en un pipe anónimo, con plazo.
bool ReadWithTimeout(HANDLE h, void* data, std::size_t size, int timeoutMs)
{
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
	for (;;)
	{
		DWORD available = 0;
		if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) return false;
		if (available >= size) return ReadExact(h, data, size);
		if (Clock::now() >= deadline) return false;
		Sleep(5);
	}
}
#else
bool ReadWithTimeout(int fd, void* data, std::size_t size, int timeoutMs)
{
	const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
	std::uint8_t* p = static_cast<std::uint8_t*>(data);
	while (size > 0)
	{
		const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
		if (left <= 0) return false;
		pollfd pfd{ fd, POLLIN, 0 };
		const int r = ::poll(&pfd, 1, left);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) return false;
		const ssize_t got = ::read(fd, p, size);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) return false;
		p += got;
		size -= static_cast<std::size_t>(got);
	}
	return true;
}
#endif

} // namespace

bool IsGameRole()
{
	return s_GameRole || std::getenv(kPipeVar) != nullptr;
}

void StartGameRole()
{
	const char* pipe = std::getenv(kPipeVar);
	const char* parent = std::getenv(kParentVar);
	if (!pipe) return;
	s_GameRole = true;
	const unsigned long long parentPid = parent ? std::strtoull(parent, nullptr, 10) : 0;

#ifdef _WIN32
	unsigned long long read = 0, write = 0;
	if (std::sscanf(pipe, "%llu,%llu", &read, &write) == 2)
	{
		s_FromGuardian = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(read));
		s_ToGuardian = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(write));
		// Lo que abra el juego (el launcher, por ejemplo) no los tiene que heredar.
		SetHandleInformation(s_FromGuardian, HANDLE_FLAG_INHERIT, 0);
		SetHandleInformation(s_ToGuardian, HANDLE_FLAG_INHERIT, 0);
	}
	SetEnvironmentVariableA(kPipeVar, nullptr);
	SetEnvironmentVariableA(kParentVar, nullptr);
	_putenv_s(kPipeVar, "");
	_putenv_s(kParentVar, "");

	// Si el guardián muere, el juego se cierra (el pipe solo no avisa hasta el
	// próximo login).
	HANDLE guardian = parentPid ? OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(parentPid)) : nullptr;
	if (guardian)
	{
		std::thread([guardian]() {
			WaitForSingleObject(guardian, INFINITE);
			DbgLogPublic("MU-Guard: el guardian termino; se cierra el juego");
			TerminateProcess(GetCurrentProcess(), 0);
		}).detach();
	}
#else
	s_GuardianFd = std::atoi(pipe);
	::fcntl(s_GuardianFd, F_SETFD, FD_CLOEXEC);
	::unsetenv(kPipeVar);
	::unsetenv(kParentVar);
	// Si el guardián muere, el kernel le manda SIGKILL al juego.
	::prctl(PR_SET_PDEATHSIG, SIGKILL);
	if (parentPid != 0 && static_cast<unsigned long long>(::getppid()) != parentPid) ::_exit(0);
#endif
	Log("MU-Guard: juego lanzado por el guardian (pid %llu)", parentPid);
}

bool FetchTicket(const std::uint8_t transcript[32], std::uint8_t id[16], std::uint8_t mac[32])
{
	if (!s_GameRole || transcript == nullptr) return false;
	std::lock_guard<std::mutex> lock(s_GameLock);

	const std::uint8_t ask[2] = { kAskTicket, ++s_Sequence };
	std::uint8_t answer[1 + kTicketReply];
#ifdef _WIN32
	bool ok = s_ToGuardian && WriteExact(s_ToGuardian, ask, sizeof(ask));
#else
	bool ok = s_GuardianFd >= 0 && WriteExact(s_GuardianFd, ask, sizeof(ask));
#endif
	// Respuestas de pedidos anteriores (vencidos) se descartan.
	const Clock::time_point deadline = Clock::now() + std::chrono::seconds(15);
	while (ok)
	{
		const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
#ifdef _WIN32
		ok = left > 0 && ReadWithTimeout(s_FromGuardian, answer, sizeof(answer), left);
#else
		ok = left > 0 && ReadWithTimeout(s_GuardianFd, answer, sizeof(answer), left);
#endif
		if (ok && answer[0] == s_Sequence) break;
	}
	if (!ok)
	{
		DbgLogPublic("MU-Guard: el guardian no contesto el pedido de ticket");
		return false;
	}
	const std::uint8_t* reply = answer + 1;
	if (reply[0] != GuardProto::Ok) return false;

	std::uint8_t message[sizeof(GuardProto::TicketContext) - 1 + GuardProto::TranscriptSize];
	std::memcpy(message, GuardProto::TicketContext, sizeof(GuardProto::TicketContext) - 1);
	std::memcpy(message + sizeof(GuardProto::TicketContext) - 1, transcript, GuardProto::TranscriptSize);
	std::memcpy(id, reply + 1, GuardProto::TicketIdSize);
	crypto_blake2b_keyed(mac, GuardProto::MacSize, reply + 1 + GuardProto::TicketIdSize, GuardProto::TicketKeySize, message, sizeof(message));
	crypto_wipe(answer, sizeof(answer));
	return true;
}

bool Run(const char* host, unsigned short port, char** argv, int* exitCode)
{
	(void)argv;
	if (IsGameRole() || host == nullptr || *host == '\0') return false;

	ServerTrust::SetServer(std::string(Platform::UserDir()) + "Cache/" + CContentCatalog::CacheId(host, port));

	const std::uint16_t guardPort = AskConnectServer(host, port);
	if (guardPort == 0) return false;

	const std::string exe = ExecutablePath();
	std::uint8_t mainHash[GuardProto::HashSize] = {};
	if (exe.empty() || !HashFile(exe, mainHash))
	{
		Log("MU-Guard: no se pudo leer el ejecutable; sigue sin guardian");
		return false;
	}

	GuardConnection guard;
	guard.Configure(host, guardPort, mainHash);
	if (!guard.Ensure())
	{
		// Puede estar caído: lo decide el GameServer en el login (4c).
		Log("MU-Guard: no responde mu-guard en %s:%u; sigue sin guardian", host, guardPort);
		return false;
	}
	Log("MU-Guard: guardian activo (mu-guard en %s:%u); se lanza el juego", host, guardPort);

#ifdef _WIN32
	SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
	HANDLE toGameRead = nullptr, toGameWrite = nullptr, fromGameRead = nullptr, fromGameWrite = nullptr;
	if (!CreatePipe(&toGameRead, &toGameWrite, &sa, 0) || !CreatePipe(&fromGameRead, &fromGameWrite, &sa, 0))
	{
		Log("MU-Guard: no se pudo crear el pipe (%lu)", GetLastError());
		return false;
	}
	// Los extremos del guardián no se heredan.
	SetHandleInformation(toGameWrite, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(fromGameRead, HANDLE_FLAG_INHERIT, 0);

	char value[64];
	std::snprintf(value, sizeof(value), "%llu,%llu", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(toGameRead)),
	              static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(fromGameWrite)));
	SetEnvironmentVariableA(kPipeVar, value);
	std::snprintf(value, sizeof(value), "%lu", GetCurrentProcessId());
	SetEnvironmentVariableA(kParentVar, value);

	// Sólo los dos extremos del juego se heredan, ninguna otra cosa del guardián.
	SIZE_T attributeSize = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
	std::vector<std::uint8_t> attributes(attributeSize);
	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
	HANDLE inherited[2] = { toGameRead, fromGameWrite };
	PROCESS_INFORMATION pi{};
	std::wstring command = GetCommandLineW();
	wchar_t module[MAX_PATH * 4];
	GetModuleFileNameW(nullptr, module, static_cast<DWORD>(sizeof(module) / sizeof(module[0])));
	const bool started = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attributeSize) &&
		UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) &&
		CreateProcessW(module, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr, &si.StartupInfo, &pi);
	const DWORD error = started ? 0 : GetLastError();
	if (si.lpAttributeList) DeleteProcThreadAttributeList(si.lpAttributeList);
	SetEnvironmentVariableA(kPipeVar, nullptr);
	SetEnvironmentVariableA(kParentVar, nullptr);
	CloseHandle(toGameRead);
	CloseHandle(fromGameWrite);
	if (!started)
	{
		Log("MU-Guard: no se pudo lanzar el juego (%lu)", error);
		CloseHandle(toGameWrite);
		CloseHandle(fromGameRead);
		return false;
	}
	CloseHandle(pi.hThread);

	std::thread server([&]() { Serve(fromGameRead, toGameWrite, guard); });
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	// El juego terminó.  No se espera el fin del pipe: otro proceso que el juego
	// haya abierto puede tener heredada una copia de su extremo.  Se corta la
	// lectura bloqueada de Serve.
	Log("MU-Guard: el juego termino (%lu)", code);
	const HANDLE thread = static_cast<HANDLE>(server.native_handle());
	for (int i = 0; i < 50 && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT; i++)
	{
		CancelSynchronousIo(thread);
		Sleep(20);
	}
	if (WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) ExitProcess(code);
	server.join();
	CloseHandle(toGameWrite);
	CloseHandle(fromGameRead);
	*exitCode = static_cast<int>(code);
	return true;
#else
	int pair[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
	{
		Log("MU-Guard: no se pudo crear el pipe (%d)", errno);
		return false;
	}
	// El extremo del guardián no se hereda; el del juego sí.
	::fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	::setenv(kPipeVar, std::to_string(pair[1]).c_str(), 1);
	::setenv(kParentVar, std::to_string(::getpid()).c_str(), 1);

	char* defaultArgv[] = { const_cast<char*>(exe.c_str()), nullptr };
	pid_t pid = 0;
	const int spawned = ::posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv ? argv : defaultArgv, environ);
	::unsetenv(kPipeVar);
	::unsetenv(kParentVar);
	::close(pair[1]);
	if (spawned != 0)
	{
		Log("MU-Guard: no se pudo lanzar el juego (%d)", spawned);
		::close(pair[0]);
		return false;
	}

	std::thread server([&]() { Serve(pair[0], pair[0], guard); });
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	// Otro proceso que haya abierto el juego puede tener heredado su extremo: se
	// corta la lectura en vez de esperar el fin del pipe.
	::shutdown(pair[0], SHUT_RDWR);
	server.join();
	::close(pair[0]);
	const int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	Log("MU-Guard: el juego termino (%d)", code);
	*exitCode = code;
	return true;
#endif
}

} // namespace Guardian
