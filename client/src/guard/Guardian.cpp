// Ver Guardian.h.

#include "stdafx.h"
// stdafx.h redirige send() al hook del tráfico del juego (net/MuEmu.h); el pipe
// con el guardián no es tráfico del juego.
#ifdef send
#undef send
#endif
#include "guard/Guardian.h"

#include "guard/GuardBanner.h"
#include "guard/GuardDetect.h"
#include "guard/GuardProtocol.h"
#include "guard/GuardSocket.h"

#include "app/config/ServerConfig.h"
#include "assets/data/ContentCatalog.h"
#include "net/ServerTrust.h"
#include "net/crypto/SecureRandom.h"
#include "platform/UserPath.h"

extern "C" {
#include "net/crypto/monocypher.h"
}

extern "C" void DbgLogPublic(const char* msg);

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <algorithm>
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
#include <aclapi.h>
#include <rpc.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/prctl.h>
#include <sys/resource.h>
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

// ── La máquina ───────────────────────────────────────────────────────────────

// Hash de lo que identifica a la máquina (va en el saludo; mu-guard lo guarda
// con una sal propia).  Windows: UUID del SMBIOS, serie del disco del sistema y
// la MAC de la placa de red.  Linux: machine-id y el UUID del SMBIOS si se puede
// leer.  Es una señal, no una prueba: sirve para avisos y bans.
void MachineHash(std::uint8_t out[GuardProto::MachineSize])
{
	crypto_blake2b_ctx ctx;
	crypto_blake2b_init(&ctx, GuardProto::MachineSize);
	crypto_blake2b_update(&ctx, reinterpret_cast<const std::uint8_t*>(GuardProto::MachineContext), sizeof(GuardProto::MachineContext) - 1);
	auto add = [&](std::uint8_t tag, const void* data, std::size_t size) {
		std::uint8_t head[5] = { tag };
		GuardProto::PutLe32(head + 1, static_cast<std::uint32_t>(size));
		crypto_blake2b_update(&ctx, head, sizeof(head));
		crypto_blake2b_update(&ctx, static_cast<const std::uint8_t*>(data), size);
	};
#ifdef _WIN32
	// SMBIOS: la estructura de tipo 1 trae el UUID en el desplazamiento 8.
	const UINT size = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
	if (size > 8)
	{
		std::vector<std::uint8_t> table(size);
		if (GetSystemFirmwareTable('RSMB', 0, table.data(), size) == size)
		{
			const std::uint32_t length = GuardProto::GetLe32(table.data() + 4);
			const std::uint8_t* at = table.data() + 8;
			const std::uint8_t* end = at + (length < size - 8 ? length : size - 8);
			while (at + 4 <= end)
			{
				const std::uint8_t type = at[0], header = at[1];
				if (header < 4 || at + header > end) break;
				if (type == 1 && header >= 0x19)
				{
					add(1, at + 8, 16);
					break;
				}
				const std::uint8_t* next = at + header;   // las cadenas terminan en dos ceros
				while (next + 1 < end && (next[0] != 0 || next[1] != 0)) next++;
				at = next + 2;
				if (type == 127) break;
			}
		}
	}
	char windows[MAX_PATH];
	if (GetSystemWindowsDirectoryA(windows, MAX_PATH) >= 3)
	{
		const char root[4] = { windows[0], ':', '\\', 0 };
		DWORD serial = 0;
		if (GetVolumeInformationA(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) add(2, &serial, sizeof(serial));
	}
	UUID uuid;
	if (UuidCreateSequential(&uuid) == RPC_S_OK) add(3, uuid.Data4 + 2, 6);   // el nodo es la MAC
#else
	auto file = [&](std::uint8_t tag, const char* path) {
		FILE* f = std::fopen(path, "rb");
		if (!f) return false;
		char text[128];
		const std::size_t n = std::fread(text, 1, sizeof(text), f);
		std::fclose(f);
		std::size_t used = 0;
		while (used < n && text[used] != '\n' && text[used] != '\r') used++;
		if (used == 0) return false;
		add(tag, text, used);
		return true;
	};
	if (!file(1, "/etc/machine-id")) file(1, "/var/lib/dbus/machine-id");
	file(2, "/sys/class/dmi/id/product_uuid");
#endif
	crypto_blake2b_final(&ctx, out);
}

// Fecha y hora local, para el banner ("dd/mm/aaaa hh:mm").
std::string LocalTime(std::int64_t when)
{
	const std::time_t t = static_cast<std::time_t>(when);
	std::tm local{};
#ifdef _WIN32
	localtime_s(&local, &t);
#else
	localtime_r(&t, &local);
#endif
	char text[32];
	std::strftime(text, sizeof(text), "%d/%m/%Y %H:%M", &local);
	return text;
}

// ── Mensajes con mu-guard ────────────────────────────────────────────────────

// Respuesta del server a una detección que cierra el juego (Notice).
struct GuardNotice
{
	int number = 0;   // 0: sin número (no quedó guardado o no suma aviso)
	int limit = 0;
	std::uint8_t banned = 0;   // GuardProto::BanFlag
	std::int64_t until = 0;
};

// Lo que manda mu-guard sin que se le pida (desafíos, veredictos).
struct Push
{
	std::uint8_t type = 0;
	std::vector<std::uint8_t> payload;
};

class GuardConnection
{
public:
	void Configure(const std::string& host, std::uint16_t port, const std::uint8_t mainHash[GuardProto::HashSize])
	{
		this->m_Host = host;
		this->m_Port = port;
		std::memcpy(this->m_MainHash, mainHash, sizeof(this->m_MainHash));
		// Un id por guardián: mu-guard lo sigue aunque se reconecte.
		if (!SecureRandomBytes(this->m_Id, sizeof(this->m_Id))) std::memset(this->m_Id, 0, sizeof(this->m_Id));
		MachineHash(this->m_Machine);
	}

	// Conecta y saluda si hace falta.
	bool Ensure()
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
		if (this->m_Socket.IsOpen()) return true;
		this->m_Pending.clear();
		this->m_Inbox.clear();   // desafíos de una conexión anterior: ya no valen
		if (!this->m_Socket.Open(this->m_Host, this->m_Port, 3000, Trust)) return false;

		std::uint8_t hello[GuardProto::HelloSize];
		hello[0] = static_cast<std::uint8_t>(GuardProto::Version & 0xFF);
		hello[1] = static_cast<std::uint8_t>(GuardProto::Version >> 8);
		std::memcpy(hello + 2, this->m_MainHash, GuardProto::HashSize);
		std::memcpy(hello + 2 + GuardProto::HashSize, this->m_Id, GuardProto::GuardianIdSize);
		std::memcpy(hello + 2 + GuardProto::HashSize + GuardProto::GuardianIdSize, this->m_Machine, GuardProto::MachineSize);

		std::vector<std::uint8_t> reply;
		if (!this->Exchange(GuardProto::Hello, hello, sizeof(hello), GuardProto::HelloResult, reply) || reply.empty() || reply[0] != GuardProto::Ok)
		{
			if (!reply.empty() && reply[0] == GuardProto::UnknownBuild)
				Log("MU-Guard: el server no acepta este build del cliente (no esta en Data/Guard/Builds)");
			else if (reply.size() >= GuardProto::HelloResultTooManySize && reply[0] == GuardProto::TooMany)
			{
				this->m_TooMany = reply[2] != 0 ? reply[2] : 1;
				Log("MU-Guard: ya hay %u juegos abiertos en esta PC (el maximo del server)", static_cast<unsigned>(this->m_TooMany));
			}
			else if (reply.size() >= GuardProto::HelloResultBannedSize && reply[0] == GuardProto::Banned)
			{
				this->m_BanFlags = reply[2];
				this->m_BanUntil = static_cast<std::int64_t>(GuardProto::GetLe64(reply.data() + 3));
				Log("MU-Guard: el server tiene baneada esta maquina o esta IP (%u)", static_cast<unsigned>(reply[2]));
			}
			else
				Log("MU-Guard: mu-guard no acepto el saludo");
			this->m_Socket.Close();
			return false;
		}
		this->m_Options = reply.size() >= 2 ? reply[1] : 0;
		return true;
	}

	// Opciones del server (GuardProto::HelloOption), del último saludo.
	std::uint8_t Options() const { return this->m_Options.load(); }

	// El saludo fue rechazado por un ban (GuardProto::BanFlag; 0 si no).
	std::uint8_t BanFlags() const { return this->m_BanFlags.load(); }
	std::int64_t BanUntil() const { return this->m_BanUntil.load(); }

	// El saludo fue rechazado por juegos abiertos de más: el máximo (0 si no).
	std::uint8_t TooMany() const { return this->m_TooMany.load(); }

	// Espera la respuesta del server a una detección que cierra el juego.
	bool AwaitNotice(GuardNotice& notice, int ms)
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
		const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(ms);
		for (;;)
		{
			for (const Push& push : this->m_Inbox)
			{
				if (push.type == GuardProto::Notice && push.payload.size() == GuardProto::NoticeSize)
				{
					notice.number = push.payload[0];
					notice.limit = push.payload[1];
					notice.banned = push.payload[2];
					notice.until = static_cast<std::int64_t>(GuardProto::GetLe64(push.payload.data() + 3));
					return true;
				}
			}
			if (!this->m_Socket.IsOpen()) return false;
			Push push;
			int popped;
			while ((popped = GuardProto::Pop(this->m_Pending, push.type, push.payload)) > 0) this->m_Inbox.push_back(push);
			if (popped < 0) return false;
			const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
			if (left <= 0) return false;
			if (!this->m_Socket.Receive(this->m_Pending, left)) return false;
		}
	}

	bool Ticket(std::uint8_t out[kTicketReply])
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
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

	// Reconecta si hace falta y junta lo que mandó mu-guard por su cuenta.
	// false si no hay conexión.
	bool Poll(std::vector<Push>& out)
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
		if (!this->Ensure()) return false;
		while (this->m_Socket.Receive(this->m_Pending, 0)) {}
		if (!this->m_Socket.IsOpen()) return false;
		Push push;
		int popped;
		while ((popped = GuardProto::Pop(this->m_Pending, push.type, push.payload)) > 0) this->m_Inbox.push_back(push);
		if (popped < 0)
		{
			this->m_Socket.Close();
			return false;
		}
		// El Notice queda para AwaitNotice: llega junto con el veredicto.
		std::vector<Push> kept;
		for (Push& push : this->m_Inbox) (push.type == GuardProto::Notice ? kept : out).push_back(std::move(push));
		this->m_Inbox.swap(kept);
		return true;
	}

	// Respuesta a un desafío (no tiene respuesta).
	bool Answer(const std::uint8_t answer[GuardProto::ChallengeAnswerSize])
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
		std::vector<std::uint8_t> message;
		GuardProto::Append(message, GuardProto::ChallengeAnswer, answer, GuardProto::ChallengeAnswerSize);
		return this->m_Socket.IsOpen() && this->m_Socket.Send(message.data(), message.size(), 3000);
	}

	// Le cuenta a mu-guard lo que se detectó (no tiene respuesta).
	bool Report(const Guard::Finding& finding)
	{
		return this->Send(finding, static_cast<std::uint8_t>(finding.closes ? 1 : 0));
	}

	// Lo que cerró el juego en un arranque anterior, sin conexión (SavePending).
	bool ReportPrevious(const Guard::Finding& finding)
	{
		return this->Send(finding, GuardProto::ReportPrevious);
	}

	bool Send(const Guard::Finding& finding, std::uint8_t closes)
	{
		std::lock_guard<std::recursive_mutex> lock(this->m_Lock);
		std::vector<std::uint8_t> body{ finding.code, closes };
		body.insert(body.end(), finding.detail.begin(), finding.detail.end());
		std::vector<std::uint8_t> message;
		GuardProto::Append(message, GuardProto::Report, body.data(), body.size());
		for (int attempt = 0; attempt < 2; attempt++)
		{
			if (!this->Ensure()) return false;
			if (this->m_Socket.Send(message.data(), message.size(), 3000)) return true;
		}
		return false;
	}

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
				this->m_Inbox.push_back({ got, payload });   // un desafío o un veredicto: para la vigilancia
				continue;
			}
			const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
			if (left <= 0 || !this->m_Socket.Receive(this->m_Pending, left)) return false;
		}
	}

	std::recursive_mutex m_Lock;   // tickets (hilo del pipe) y reportes (vigilancia)
	std::atomic<std::uint8_t> m_Options{ 0 };
	std::atomic<std::uint8_t> m_BanFlags{ 0 };
	std::atomic<std::int64_t> m_BanUntil{ 0 };
	std::atomic<std::uint8_t> m_TooMany{ 0 };
	std::uint8_t m_Machine[GuardProto::MachineSize] = {};
	std::string m_Host;
	std::uint16_t m_Port = 0;
	std::uint8_t m_MainHash[GuardProto::HashSize] = {};
	std::uint8_t m_Id[GuardProto::GuardianIdSize] = {};
	Guard::SecureSocket m_Socket;
	std::vector<std::uint8_t> m_Pending;
	std::vector<Push> m_Inbox;
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

// Qué se detectó, para el banner (el detalle técnico va al server).  El server
// puede cambiar cada nombre (Data/Guard/Messages.toml).
std::string ThreatName(const Guard::Detector& detector, const Guard::Finding& finding, int language)
{
	const std::string custom = detector.Name(finding.code, language);
	if (!custom.empty()) return custom;
	static const char* const kNames[3][7] = {
		{ "un depurador", "un editor de memoria", "Cheat Engine", "el juego modificado", "una herramienta prohibida", "un archivo del juego modificado",
		  "MU-Guard detenido" },
		{ "A debugger", "A memory editor", "Cheat Engine", "A modified game", "A forbidden tool", "A modified game file", "A stopped MU-Guard" },
		{ "Um depurador", "Um editor de mem\xC3\xB3ria", "Cheat Engine", "O jogo modificado", "Uma ferramenta proibida", "Um arquivo do jogo modificado",
		  "O MU-Guard parado" },
	};
	const int lang = (language >= 0 && language < 3) ? language : 0;
	switch (finding.code)
	{
	case GuardProto::Debugger: return kNames[lang][0];
	case GuardProto::MemEdit:
	case GuardProto::MemScan: return kNames[lang][1];
	case GuardProto::Patch: return kNames[lang][3];
	case GuardProto::Tool: return kNames[lang][4];
	case GuardProto::Files: return kNames[lang][5];
	case GuardProto::Killed: return kNames[lang][6];
	default: return kNames[lang][2];
	}
}

// El aviso del server, para el banner ("Aviso 2 de 3." o hasta cuándo queda
// bloqueado).  Vacío si no hay número.
std::string NoticeText(const GuardNotice& notice, int language)
{
	static const char* const kWarning[3] = { "Aviso {0} de {1}.", "Warning {0} of {1}.", "Aviso {0} de {1}." };
	static const char* const kWarningNoLimit[3] = { "Aviso {0}.", "Warning {0}.", "Aviso {0}." };
	static const char* const kBannedUntil[3] = { "Qued\xC3\xA1s bloqueado hasta el {0}.", "You are banned until {0}.", "Voc\xC3\xAA est\xC3\xA1 bloqueado at\xC3\xA9 {0}." };
	static const char* const kBannedForever[3] = { "Qued\xC3\xA1s bloqueado de forma permanente.", "You are permanently banned.", "Voc\xC3\xAA est\xC3\xA1 bloqueado permanentemente." };
	const int lang = (language >= 0 && language < 3) ? language : 0;
	auto fill = [](std::string text, const std::string& a, const std::string& b) {
		std::size_t at = text.find("{0}");
		if (at != std::string::npos) text.replace(at, 3, a);
		at = text.find("{1}");
		if (at != std::string::npos) text.replace(at, 3, b);
		return text;
	};
	if (notice.banned != 0)
		return notice.until == 0 ? kBannedForever[lang] : fill(kBannedUntil[lang], LocalTime(notice.until), "");
	if (notice.number == 0) return std::string();
	return notice.limit > 0 ? fill(kWarning[lang], std::to_string(notice.number), std::to_string(notice.limit))
	                        : fill(kWarningNoLimit[lang], std::to_string(notice.number), "");
}

// Juegos abiertos de más en esta PC, para el banner.
std::string TooManyText(unsigned limit, int language)
{
	static const char* const kOne[3] = { "Este servidor permite un solo juego abierto por PC.",
		"This server allows only one game open per PC.", "Este servidor permite s\xC3\xB3 um jogo aberto por PC." };
	static const char* const kMany[3] = { "Este servidor permite {0} juegos abiertos por PC.",
		"This server allows {0} games open per PC.", "Este servidor permite {0} jogos abertos por PC." };
	const int lang = (language >= 0 && language < 3) ? language : 0;
	std::string text = limit <= 1 ? kOne[lang] : kMany[lang];
	const std::size_t at = text.find("{0}");
	if (at != std::string::npos) text.replace(at, 3, std::to_string(limit));
	return text;
}

// La máquina o la IP baneadas en el saludo, para el banner.
std::string BlockedText(std::uint8_t flags, std::int64_t until, int language)
{
	static const char* const kWho[3][2] = {
		{ "Esta PC", "Esta conexi\xC3\xB3n" }, { "This PC", "This connection" }, { "Este PC", "Esta conex\xC3\xA3o" } };
	static const char* const kUntil[3] = { "{0} est\xC3\xA1 bloqueada hasta el {1}.", "{0} is banned until {1}.", "{0} est\xC3\xA1 bloqueada at\xC3\xA9 {1}." };
	static const char* const kForever[3] = { "{0} est\xC3\xA1 bloqueada de forma permanente.", "{0} is permanently banned.", "{0} est\xC3\xA1 bloqueada permanentemente." };
	const int lang = (language >= 0 && language < 3) ? language : 0;
	std::string text = until == 0 ? kForever[lang] : kUntil[lang];
	std::size_t at = text.find("{0}");
	if (at != std::string::npos) text.replace(at, 3, kWho[lang][(flags & GuardProto::BanMachine) ? 0 : 1]);
	at = text.find("{1}");
	if (at != std::string::npos) text.replace(at, 3, LocalTime(until));
	return text;
}

// Desafío de memoria (paso 4): el hash de los rangos pedidos del código del
// juego, tal como está cargado.  Se guarda hasta poder contestarlo (el juego
// puede no haber terminado de cargar).
struct Vigil
{
	bool pending = false;
	std::vector<std::uint8_t> challenge;   // el payload del Challenge
};

// Marca que lee mu-patch: un build con MU-Guard y sin la clave del server
// compilada (ServerConfig::ServerPublicKey) no se publica, porque sin esa clave
// un proxy puede hacerse pasar por el server y el ticket no protege nada.  Sólo
// queda en el ejecutable la del caso que corresponde.
template <bool NoKey> struct KeyMark { static constexpr char text[] = "MuGuardKey1:ok"; };
template <> struct KeyMark<true> { static constexpr char text[] = "MuGuardKey1:none"; };
constexpr bool kNoServerKey = ServerConfig::ServerPublicKey[0] == '\0';

bool AnswerChallenge(Guard::Detector& detector, GuardConnection& guard, const std::vector<std::uint8_t>& challenge)
{
	using namespace GuardProto;
	if (challenge.size() < 4 + NonceSize + 1) return true;   // mal formado: se descarta
	const std::size_t count = challenge[4 + NonceSize];
	if (count == 0 || count > MaxChallengeRanges || challenge.size() != 5 + NonceSize + count * 8) return true;

	std::uint64_t base = 0;
	if (!detector.ImageBase(base)) return false;

	crypto_blake2b_ctx ctx;
	crypto_blake2b_keyed_init(&ctx, HashSize, challenge.data() + 4, NonceSize);
	crypto_blake2b_update(&ctx, reinterpret_cast<const std::uint8_t*>(ChallengeContext), sizeof(ChallengeContext) - 1);
	std::vector<std::uint8_t> bytes;
	for (std::size_t i = 0; i < count; i++)
	{
		const std::uint32_t rva = GetLe32(challenge.data() + 5 + NonceSize + i * 8);
		const std::uint32_t size = GetLe32(challenge.data() + 9 + NonceSize + i * 8);
		if (size == 0 || size > MaxChallengeRange) return true;
		bytes.resize(size);
		// Lo que no se puede leer va en ceros: el server lo da por mal.
		if (!detector.Read(base + rva, bytes.data(), size)) std::fill(bytes.begin(), bytes.end(), 0);
		crypto_blake2b_update(&ctx, bytes.data(), size);
	}
	std::uint8_t answer[ChallengeAnswerSize];
	std::memcpy(answer, challenge.data(), 4);
	PutLe64(answer + 4, base);
	crypto_blake2b_final(&ctx, answer + 12);
	std::uint32_t life[LifeCounters];
	detector.Life(life);
	for (std::size_t i = 0; i < LifeCounters; i++) PutLe32(answer + 12 + HashSize + i * 4, life[i]);
	guard.Answer(answer);
	return true;
}

// Lo que cerró el juego sin poder avisarle a mu-guard (sin conexión) queda en
// User/Cache/guard-pending.txt ("detección TAB detalle" por línea) y se manda
// en el próximo arranque.  Se puede borrar: es lo mejor posible.
std::string PendingPath()
{
	return std::string(Platform::UserDir()) + "Cache/guard-pending.txt";
}

void SavePending(const Guard::Finding& finding)
{
	FILE* file = std::fopen(PendingPath().c_str(), "a");
	if (!file) return;
	std::string detail = finding.detail;
	std::replace(detail.begin(), detail.end(), '\n', ' ');
	std::replace(detail.begin(), detail.end(), '\r', ' ');
	std::replace(detail.begin(), detail.end(), '\t', ' ');
	std::fprintf(file, "%u\t%s\n", static_cast<unsigned>(finding.code), detail.c_str());
	std::fclose(file);
	Log("MU-Guard: sin conexion con mu-guard; la deteccion se manda en el proximo arranque");
}

template <class Connection>
void SendPending(Connection& guard)
{
	FILE* file = std::fopen(PendingPath().c_str(), "r");
	if (!file) return;
	std::vector<Guard::Finding> pending;
	char line[GuardProto::MaxDetail + 16];
	while (pending.size() < 8 && std::fgets(line, sizeof(line), file))
	{
		char* tab = std::strchr(line, '\t');
		if (!tab) continue;
		Guard::Finding finding;
		finding.code = static_cast<std::uint8_t>(std::atoi(line));
		finding.detail.assign(tab + 1);
		while (!finding.detail.empty() && (finding.detail.back() == '\n' || finding.detail.back() == '\r')) finding.detail.pop_back();
		pending.push_back(finding);
	}
	std::fclose(file);
	bool sent = true;
	for (const Guard::Finding& finding : pending) sent = guard.ReportPrevious(finding) && sent;
	if (sent)
	{
		std::remove(PendingPath().c_str());
		Log("MU-Guard: %u deteccion(es) de un arranque anterior enviadas", static_cast<unsigned>(pending.size()));
	}
}

// Una pasada de los detectores: todo se reporta; si algo cierra el juego, lo
// devuelve.  También atiende lo que manda mu-guard.
bool Watch(Guard::Detector& detector, GuardConnection& guard, Vigil& vigil, Guard::Finding& closing)
{
	std::vector<Push> pushes;
	guard.Poll(pushes);
	for (const Push& push : pushes)
	{
		if (push.type == GuardProto::Rules && !push.payload.empty())
		{
			detector.Rule(push.payload[0], std::string(push.payload.begin() + 1, push.payload.end()));
		}
		else if (push.type == GuardProto::Challenge)
		{
			vigil.pending = true;
			vigil.challenge = push.payload;
		}
		else if (push.type == GuardProto::Verdict && !push.payload.empty())
		{
			// Orden del server: el juego se cierra.
			closing.code = push.payload[0];
			closing.closes = true;
			closing.detail.assign(push.payload.begin() + 1, push.payload.end());
			Log("MU-Guard: mu-guard ordeno cerrar el juego (%u): %s", static_cast<unsigned>(closing.code), closing.detail.c_str());
			return true;
		}
	}
	if (vigil.pending && AnswerChallenge(detector, guard, vigil.challenge)) vigil.pending = false;

	std::vector<Guard::Finding> found;
	detector.Scan(found);
	bool close = false;
	for (const Guard::Finding& finding : found)
	{
		Log("MU-Guard: deteccion %u (%s): %s", static_cast<unsigned>(finding.code), finding.closes ? "cierra" : "se reporta", finding.detail.c_str());
		if (!guard.Report(finding) && finding.closes) SavePending(finding);
		if (finding.closes && !close)
		{
			closing = finding;
			close = true;
		}
	}
	return close;
}

constexpr int kWatchMs = 2000;

// Idioma del banner: el del juego (Config.ini [Language] LangSelection), que
// todavía no se leyó.  0 castellano, 1 inglés, 2 portugués.
int LanguageIndex()
{
	FILE* file = std::fopen((std::string(Platform::UserDir()) + "Config.ini").c_str(), "r");
	if (!file) return 0;
	char line[256];
	int language = 0;
	while (std::fgets(line, sizeof(line), file))
	{
		if (std::strncmp(line, "LangSelection=", 14) != 0) continue;
		if (std::strncmp(line + 14, "Eng", 3) == 0) language = 1;
		else if (std::strncmp(line + 14, "Por", 3) == 0) language = 2;
		break;
	}
	std::fclose(file);
	return language;
}

#ifdef _WIN32
// DACL del guardián y del juego (diseño 1.2 y 3.1): el usuario sólo puede
// terminarlos, esperarlos y consultar lo básico; SYSTEM, todo.  Leer o escribir
// la memoria, duplicar sus handles o cambiar la DACL queda denegado a cualquier
// otro programa del mismo usuario.  La entrada OWNER RIGHTS hace falta porque
// sin ella el dueño tiene WRITE_DAC implícito y podría quitarle la DACL.
// El guardián no pierde nada: lee al juego por el handle que le dio CreateProcess.
constexpr DWORD kProcessRights = PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;
constexpr DWORD kThreadRights = THREAD_TERMINATE | THREAD_QUERY_LIMITED_INFORMATION | SYNCHRONIZE;

class Descriptor
{
public:
	explicit Descriptor(DWORD rights)
	{
		HANDLE token = nullptr;
		if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
		DWORD size = 0;
		GetTokenInformation(token, TokenUser, nullptr, 0, &size);
		std::vector<std::uint8_t> buffer(size);
		LPWSTR sid = nullptr;
		if (size && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) &&
		    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid))
		{
			wchar_t sddl[512];
			swprintf(sddl, sizeof(sddl) / sizeof(sddl[0]), L"D:P(A;;0x%08lx;;;OW)(A;;0x%08lx;;;%ls)(A;;GA;;;SY)",
			         static_cast<unsigned long>(rights), static_cast<unsigned long>(rights), sid);
			ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &m_Descriptor, nullptr);
			LocalFree(sid);
		}
		CloseHandle(token);
	}

	~Descriptor() { if (m_Descriptor) LocalFree(m_Descriptor); }

	PSECURITY_DESCRIPTOR Get() const { return m_Descriptor; }

	PACL Dacl() const
	{
		BOOL present = FALSE, defaulted = FALSE;
		PACL dacl = nullptr;
		return (m_Descriptor && GetSecurityDescriptorDacl(m_Descriptor, &present, &dacl, &defaulted) && present) ? dacl : nullptr;
	}

private:
	PSECURITY_DESCRIPTOR m_Descriptor = nullptr;
};

// Para cerrar el banner cuando el juego ya muestra su ventana.
struct WindowSearch { DWORD pid; bool found; };

BOOL CALLBACK FindVisibleWindow(HWND hwnd, LPARAM data)
{
	WindowSearch* search = reinterpret_cast<WindowSearch*>(data);
	DWORD pid = 0;
	GetWindowThreadProcessId(hwnd, &pid);
	if (pid == search->pid && IsWindowVisible(hwnd))
	{
		search->found = true;
		return FALSE;
	}
	return TRUE;
}

bool HasVisibleWindow(DWORD pid)
{
	WindowSearch search{ pid, false };
	EnumWindows(FindVisibleWindow, reinterpret_cast<LPARAM>(&search));
	return search.found;
}

struct ServeArgs { HANDLE in, out; GuardConnection* guard; };

DWORD WINAPI ServeThread(LPVOID p)
{
	ServeArgs* a = static_cast<ServeArgs*>(p);
	Serve(a->in, a->out, *a->guard);
	return 0;
}
#endif

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
	if (kNoServerKey)
		Log("MU-Guard: este build no tiene la clave del server compilada (%s); sirve para probar, "
		    "pero mu-patch no lo publica", KeyMark<kNoServerKey>::text);
	else
		Log("MU-Guard: clave del server compilada (%s)", KeyMark<kNoServerKey>::text);

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

	// El banner aparece recién ahora: sin MU-Guard en el server no se muestra.
	// Cada paso queda en pantalla un mínimo (el SECUENCIA del template) para que
	// un chequeo instantáneo no haga parpadear el texto.
	const int language = LanguageIndex();
	const std::string gameDir = exe.substr(0, exe.find_last_of("\\/") + 1);
	Guard::Banner banner;
	banner.Open(language);
	banner.Step(Guard::BannerStep::Network);
	std::atomic<int> connected{ -1 };
	std::thread hello([&]() { connected = guard.Ensure() ? 1 : 0; });
	const bool kept = banner.Wait([&]() { return connected.load() >= 0; }, 750);
	hello.join();
	if (!kept)
	{
		Log("MU-Guard: se cerro el banner; no se abre el juego");
		*exitCode = 0;
		return true;
	}
	if (connected.load() != 1 && guard.TooMany() != 0)
	{
		// Ya están todos los juegos que permite el server en esta PC.
		banner.Refused(language, TooManyText(guard.TooMany(), language));
		banner.Close();
		*exitCode = 1;
		return true;
	}
	if (connected.load() != 1 && guard.BanFlags() != 0)
	{
		// Baneado: el juego no se abre.
		banner.Blocked(language, BlockedText(guard.BanFlags(), guard.BanUntil(), language));
		banner.Close();
		*exitCode = 1;
		return true;
	}
	if (connected.load() != 1)
	{
		// Puede estar caído: lo decide el GameServer en el login (4c).
		banner.Close();
		Log("MU-Guard: no responde mu-guard en %s:%u; sigue sin guardian", host, guardPort);
		return false;
	}
	Log("MU-Guard: guardian activo (mu-guard en %s:%u); se lanza el juego", host, guardPort);
	SendPending(guard);
	banner.Step(Guard::BannerStep::Launch);

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
	// El guardián toma la DACL antes de crear al juego: si no, otro programa le
	// duplica el handle del juego (PROCESS_DUP_HANDLE) y se saltea todo.
	const Descriptor processSd(kProcessRights), threadSd(kThreadRights);
	if (!processSd.Get() || !threadSd.Get() ||
	    SetSecurityInfo(GetCurrentProcess(), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
	                    nullptr, nullptr, processSd.Dacl(), nullptr) != ERROR_SUCCESS)
		Log("MU-Guard: no se pudo proteger el guardian (%lu)", GetLastError());
	SECURITY_ATTRIBUTES processAttributes{ sizeof(SECURITY_ATTRIBUTES), processSd.Get(), FALSE };
	SECURITY_ATTRIBUTES threadAttributes{ sizeof(SECURITY_ATTRIBUTES), threadSd.Get(), FALSE };
	std::wstring command = GetCommandLineW();
	wchar_t module[MAX_PATH * 4];
	GetModuleFileNameW(nullptr, module, static_cast<DWORD>(sizeof(module) / sizeof(module[0])));
	const bool started = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attributeSize) &&
		UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr) &&
		CreateProcessW(module, command.data(), processSd.Get() ? &processAttributes : nullptr, threadSd.Get() ? &threadAttributes : nullptr,
		               TRUE, EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr, nullptr, &si.StartupInfo, &pi);
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
		banner.Close();
		return false;
	}

	// Hilo de Win32 y no std::thread: hace falta su HANDLE para cortarle la
	// lectura (con MinGW, native_handle() no es un HANDLE).
	ServeArgs args{ fromGameRead, toGameWrite, &guard };
	const HANDLE thread = CreateThread(nullptr, 0, ServeThread, &args, 0, nullptr);
	// Se creó suspendido: ya tiene su DACL, el guardián lo puede atender y las
	// páginas señuelo quedan puestas antes de que corra.
	Guard::Detector detector;
	detector.SetMemScanCloses((guard.Options() & GuardProto::MemScanCloses) != 0);
	detector.SetToolCloses((guard.Options() & GuardProto::ToolCloses) != 0);
	detector.SetFilesCloses((guard.Options() & GuardProto::FilesCloses) != 0);
	detector.Attach(pi.hProcess, pi.hThread, pi.dwProcessId, gameDir);
	detector.SetThreadDacl(threadSd.Dacl());
	Vigil vigil;
	ResumeThread(pi.hThread);

	// El banner se va cuando el juego muestra su ventana (o termina): así no
	// queda un hueco sin nada en pantalla.  Como mucho 30 s.
	const auto launched = std::chrono::steady_clock::now();
	banner.Wait([&]() {
		return WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0 || HasVisibleWindow(pi.dwProcessId) ||
		       std::chrono::steady_clock::now() - launched > std::chrono::seconds(30);
	}, 1200);
	banner.Close();

	// Vigilancia: el guardián queda por debajo de lo normal, no frena al juego.
	SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
	Guard::Finding closing;
	bool threat = false;
	while (WaitForSingleObject(pi.hProcess, kWatchMs) == WAIT_TIMEOUT)
	{
		if (Watch(detector, guard, vigil, closing))
		{
			threat = true;
			TerminateProcess(pi.hProcess, 1);
			WaitForSingleObject(pi.hProcess, 5000);
			break;
		}
	}
	DWORD code = 0;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	if (threat)
	{
		SetPriorityClass(GetCurrentProcess(), NORMAL_PRIORITY_CLASS);
		GuardNotice notice;
		if (guard.AwaitNotice(notice, 2000))
			Log("MU-Guard: aviso %d de %d (baneado %u)", notice.number, notice.limit, static_cast<unsigned>(notice.banned));
		banner.Threat(language, ThreatName(detector, closing, language), NoticeText(notice, language));
		code = 1;
	}
	// El juego terminó.  No se espera el fin del pipe: otro proceso que el juego
	// haya abierto puede tener heredada una copia de su extremo.  Se corta la
	// lectura bloqueada de Serve.
	Log("MU-Guard: el juego termino (%lu)", code);
	for (int i = 0; thread && i < 50 && WaitForSingleObject(thread, 0) == WAIT_TIMEOUT; i++)
	{
		CancelSynchronousIo(thread);
		Sleep(20);
	}
	if (thread && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) ExitProcess(code);
	if (thread) CloseHandle(thread);
	CloseHandle(toGameWrite);
	CloseHandle(fromGameRead);
	*exitCode = static_cast<int>(code);
	return true;
#else
	int pair[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0)
	{
		Log("MU-Guard: no se pudo crear el pipe (%d)", errno);
		banner.Close();
		return false;
	}
	// Nadie del mismo usuario lee la memoria del guardián (tiene las claves de
	// los tickets).  El juego no hereda esto: el exec lo vuelve a dejar legible,
	// y en Linux lo protege Yama (ptrace_scope >= 1: sólo un ancestro).
	::prctl(PR_SET_DUMPABLE, 0);
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
		banner.Close();
		return false;
	}

	std::thread server([&]() { Serve(pair[0], pair[0], guard); });
	Guard::Detector detector;
	detector.SetMemScanCloses((guard.Options() & GuardProto::MemScanCloses) != 0);
	detector.SetToolCloses((guard.Options() & GuardProto::ToolCloses) != 0);
	detector.SetFilesCloses((guard.Options() & GuardProto::FilesCloses) != 0);
	detector.Attach(nullptr, nullptr, static_cast<unsigned long>(pid), gameDir);
	Vigil vigil;
	banner.Wait([]() { return true; }, 1200);
	banner.Close();

	// Vigilancia, con el guardián por debajo de lo normal.
	::setpriority(PRIO_PROCESS, 0, 5);
	Guard::Finding closing;
	bool threat = false;
	int status = 0;
	for (;;)
	{
		const pid_t done = ::waitpid(pid, &status, WNOHANG);
		if (done == pid || (done < 0 && errno != EINTR)) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(kWatchMs));
		if (::waitpid(pid, &status, WNOHANG) == pid) break;
		if (Watch(detector, guard, vigil, closing))
		{
			threat = true;
			::kill(pid, SIGKILL);
			while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
			break;
		}
	}
	// Otro proceso que haya abierto el juego puede tener heredado su extremo: se
	// corta la lectura en vez de esperar el fin del pipe.
	::shutdown(pair[0], SHUT_RDWR);
	server.join();
	::close(pair[0]);
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
	if (threat)
	{
		::setpriority(PRIO_PROCESS, 0, 0);
		GuardNotice notice;
		if (guard.AwaitNotice(notice, 2000))
			Log("MU-Guard: aviso %d de %d (baneado %u)", notice.number, notice.limit, static_cast<unsigned>(notice.banned));
		banner.Threat(language, ThreatName(detector, closing, language), NoticeText(notice, language));
		code = 1;
	}
	Log("MU-Guard: el juego termino (%d)", code);
	*exitCode = code;
	return true;
#endif
}

} // namespace Guardian
