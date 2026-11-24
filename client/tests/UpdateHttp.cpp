// Transporte HTTP del actualizador (src/update/HttpTransport) y flujo del
// launcher (src/launcher, sin la ventana) contra un server
// HTTP mínimo en 127.0.0.1: rangos, redirecciones, un server que ignora el
// Range, cortes y una instalación de punta a punta que retoma después de un
// corte.  También el armado y la validación de canales (ChannelFetch) por HTTP.

#include "IniDocument.h"
#include "Lang.h"
#include "LauncherConfig.h"
#include "OptionsModel.h"
#include "PatchBuilder.h"
#include "SelfUpdate.h"
#include "platform/RunningLock.h"
#include "ServerAnnounce.h"
#include "UpdateFlow.h"

#include "update/Announce.h"
#include "update/ChannelFetch.h"
#include "update/FileUtil.h"
#include "update/Hash.h"
#include "update/HttpTransport.h"
#include "update/InstallIndex.h"
#include "update/InstallPlan.h"
#include "update/Installer.h"
#include "update/Manifest.h"
#include "update/Signing.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Update;

namespace {

int s_Failures = 0;

void Require(bool ok, const char* what)
{
	if (!ok)
	{
		std::printf("FALLA: %s\n", what);
		++s_Failures;
	}
}

std::vector<std::uint8_t> Random(std::size_t size, unsigned seed)
{
	std::mt19937 rng(seed);
	std::vector<std::uint8_t> v(size);
	for (auto& b : v) b = static_cast<std::uint8_t>(rng());
	return v;
}

void Write(const std::string& path, const std::vector<std::uint8_t>& data)
{
	Require(WriteFileAtomic(path, data.data(), data.size()), "escribir archivo de prueba");
}

#ifdef _WIN32
using Sock = SOCKET;
const Sock kNoSock = INVALID_SOCKET;
void CloseSock(Sock s) { closesocket(s); }
constexpr int kSendFlags = 0;
#else
using Sock = int;
const Sock kNoSock = -1;
void CloseSock(Sock s) { close(s); }
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

bool SendAll(Sock s, const char* data, std::size_t size)
{
	while (size)
	{
		const int chunk = static_cast<int>(size > (1 << 16) ? (1 << 16) : size);
		const int n = send(s, data, chunk, kSendFlags);
		if (n <= 0) return false;
		data += n;
		size -= static_cast<std::size_t>(n);
	}
	return true;
}

// Server HTTP/1.1 de una respuesta por conexión.  Sirve los archivos de root;
// "/redirect/<x>" responde 302 a "/<x>".
class TestServer
{
public:
	std::atomic<int> cutNext{ 0 };            // próximos cuerpos que se cortan...
	std::atomic<std::uint64_t> cutAfter{ 0 };  // ...después de tantos bytes
	std::atomic<bool> ignoreRange{ false };
	std::atomic<int> requests{ 0 };
	int port = 0;

	bool Start(const std::string& root)
	{
		m_Root = root;
		m_Listen = socket(AF_INET, SOCK_STREAM, 0);
		if (m_Listen == kNoSock) return false;
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = 0;
		if (bind(m_Listen, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(m_Listen, 16) != 0) return false;
		socklen_t len = sizeof(a);
		getsockname(m_Listen, reinterpret_cast<sockaddr*>(&a), &len);
		port = ntohs(a.sin_port);
		m_Thread = std::thread([this] { Accept(); });
		return true;
	}

	void Stop()
	{
		m_Stop = true;
		// Una conexión propia destraba el accept.
		Sock s = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(static_cast<unsigned short>(port));
		connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		CloseSock(s);
		if (m_Thread.joinable()) m_Thread.join();
		CloseSock(m_Listen);
		std::lock_guard<std::mutex> lock(m_Mutex);
		for (std::thread& t : m_Handlers) t.join();
		m_Handlers.clear();
	}

	std::string Url(const std::string& path) const { return "http://127.0.0.1:" + std::to_string(port) + path; }

private:
	void Accept()
	{
		for (;;)
		{
			Sock c = accept(m_Listen, nullptr, nullptr);
			if (m_Stop)
			{
				if (c != kNoSock) CloseSock(c);
				return;
			}
			if (c == kNoSock) continue;
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Handlers.emplace_back([this, c] { Handle(c); });
		}
	}

	void Handle(Sock c)
	{
		++requests;
		std::string req;
		char buf[4096];
		while (req.find("\r\n\r\n") == std::string::npos && req.size() < 16384)
		{
			const int n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0) break;
			req.append(buf, static_cast<std::size_t>(n));
		}
		std::string path;
		const std::size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
		if (sp1 != std::string::npos && sp2 != std::string::npos) path = req.substr(sp1 + 1, sp2 - sp1 - 1);

		std::string lower = req;
		for (char& ch : lower)
			if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
		bool ranged = false;
		std::uint64_t from = 0, to = 0;
		const std::size_t r = lower.find("\r\nrange: bytes=");
		if (r != std::string::npos && !ignoreRange)
		{
			ranged = true;
			const char* p = req.c_str() + r + 15;
			char* end = nullptr;
			from = std::strtoull(p, &end, 10);
			to = (end && *end == '-') ? std::strtoull(end + 1, nullptr, 10) : 0;
		}

		std::string head;
		std::vector<std::uint8_t> body;
		if (path.compare(0, 10, "/redirect/") == 0)
			head = "HTTP/1.1 302 Found\r\nLocation: /" + path.substr(10) + "\r\nContent-Length: 0\r\n";
		else if (path.size() < 2 || path.find("..") != std::string::npos || !ReadWholeFile(JoinPath(m_Root, path.substr(1)), body))
		{
			head = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n";
			body.clear();
		}
		else if (ranged)
		{
			if (from >= body.size())
			{
				head = "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n";
				body.clear();
			}
			else
			{
				if (to >= body.size()) to = body.size() - 1;
				const std::uint64_t total = body.size();
				body = std::vector<std::uint8_t>(body.begin() + static_cast<std::ptrdiff_t>(from), body.begin() + static_cast<std::ptrdiff_t>(to + 1));
				head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + std::to_string(from) + "-" + std::to_string(to) +
				       "/" + std::to_string(total) + "\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
			}
		}
		else
			head = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
		head += "Connection: close\r\n\r\n";

		// Sólo se cortan cuerpos más largos que el corte (los chicos pasan enteros).
		std::size_t send = body.size();
		if (body.size() > cutAfter)
		{
			int left = cutNext.load();
			while (left > 0 && !cutNext.compare_exchange_weak(left, left - 1)) {}
			if (left > 0) send = static_cast<std::size_t>(cutAfter.load());
		}
		if (SendAll(c, head.data(), head.size()) && send)
			SendAll(c, reinterpret_cast<const char*>(body.data()), send);
		CloseSock(c);
	}

	std::string m_Root;
	Sock m_Listen = kNoSock;
	std::atomic<bool> m_Stop{ false };
	std::thread m_Thread;
	std::mutex m_Mutex;
	std::vector<std::thread> m_Handlers;
};

void TestTransport(TestServer& server, const std::string& root)
{
	const auto data = Random(1 << 20, 1);
	Write(JoinPath(root, "a.bin"), data);
	HttpTransport http;
	std::string err;

	std::vector<std::uint8_t> got;
	Require(http.Get(server.Url("/a.bin"), 2 << 20, got, &err) && got == data, ("Get: " + err).c_str());
	Require(!http.Get(server.Url("/falta.bin"), 2 << 20, got, &err) && got.empty(), "Get de algo que no existe falla");
	Require(!http.Get(server.Url("/a.bin"), 1000, got, &err), "Get más grande que el tope falla");

	std::vector<std::uint8_t> slice;
	auto sink = [&](const std::uint8_t* d, std::size_t n) { slice.insert(slice.end(), d, d + n); return true; };
	Require(http.Fetch(server.Url("/a.bin"), 1000, 5000, sink, &err) &&
	            slice == std::vector<std::uint8_t>(data.begin() + 1000, data.begin() + 6000), ("Fetch por rango: " + err).c_str());
	slice.clear();
	Require(http.Fetch(server.Url("/redirect/a.bin"), 77, 300, sink, &err) &&
	            slice == std::vector<std::uint8_t>(data.begin() + 77, data.begin() + 377), ("Fetch con redirección: " + err).c_str());

	// Un server que ignora el Range: desde el principio vale, desde otro lugar no.
	server.ignoreRange = true;
	slice.clear();
	Require(!http.Fetch(server.Url("/a.bin"), 1000, 5000, sink, &err) && slice.empty(), "sin Range desde el medio: falla sin mezclar bytes");
	slice.clear();
	Require(http.Fetch(server.Url("/a.bin"), 0, 5000, sink, &err) &&
	            slice == std::vector<std::uint8_t>(data.begin(), data.begin() + 5000), ("sin Range desde el principio: " + err).c_str());
	server.ignoreRange = false;

	// Corte a mitad del cuerpo y cancelación.
	server.cutAfter = 2000;
	server.cutNext = 1;
	slice.clear();
	Require(!http.Fetch(server.Url("/a.bin"), 0, 100000, sink, &err) && slice.size() <= 2000, "corte: falla con lo que llegó");
	slice.clear();
	Require(!http.Fetch(server.Url("/a.bin"), 0, 100000, [&](const std::uint8_t*, std::size_t) { return false; }, &err),
	        "cancelado desde el sink: falla");
	Require(!http.Fetch(server.Url("/a.bin"), (1 << 20) + 10, 5, sink, &err), "rango fuera del archivo: falla");
}

void TestEndToEnd(TestServer& server, const std::string& dir, const std::string& root)
{
	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave");
	const std::string key = JoinPath(dir, "release.key");
	Require(SaveKeyFile(key, k), "guardar clave");

	const std::string in = JoinPath(dir, "in");
	const auto big = Random(3 << 20, 2), chica = Random(40000, 3);
	Write(JoinPath(in, "Content/data/big.bin"), big);
	Write(JoinPath(in, "Content/a/small.ozj"), chica);
	MuPatch::BuildOptions o;
	o.channel = "mu-linux/base";
	o.version = "0.97.50";
	o.in = in;
	o.out = JoinPath(root, "rel");
	o.keyPath = key;
	o.bundleSize = 1 << 20;
	o.level = 3;
	o.threads = 2;
	Require(MuPatch::BuildPatch(o, &err), ("build: " + err).c_str());

	HttpTransport http;
	SequenceStore seq;
	ChannelSpec spec;
	spec.channel = "mu-linux/base";
	spec.urls = { server.Url("/no-existe"), server.Url("/rel") };   // la primera falla: se prueba la siguiente
	std::memcpy(spec.publicKey, k.publicKey, kKeySize);
	spec.version = "0.97.50";
	FetchedChannel fetched;
	Require(FetchChannel(http, spec, seq, fetched, &err), ("FetchChannel por HTTP: " + err).c_str());
	Require(fetched.urls.size() == 2 && fetched.urls[0] == server.Url("/rel"), "FetchChannel: primero la URL que respondió");

	const std::string client = JoinPath(dir, "cliente");
	std::vector<ChannelSource> sources = { SourceOf(fetched, 0) };
	PlanOptions po;
	po.root = client;
	InstallIndex idx;
	Plan plan;
	Require(BuildPlan(sources, po, idx, plan, &err) && plan.files.size() == 2, ("plan: " + err).c_str());

	// El pedido del zip grande se corta una vez: se retoma desde el .part.
	server.cutAfter = 700000;
	server.cutNext = 1;
	InstallOptions io;
	io.root = client;
	io.downloadDir = JoinPath(client, "User/Cache/downloads");
	io.indexPath = JoinPath(client, "User/Cache/install.json");
	io.transport = &http;
	io.connections = 1;   // un solo hilo: el corte cae en el primer pedido
	io.backoffMs = 1;
	InstallResult res;
	const bool ok = RunPlan(sources, plan, idx, io, res);
	for (const std::string& f : res.failures) std::printf("  falla: %s\n", f.c_str());
	Require(ok && res.installed == 2, "instalar por HTTP con un corte");
	std::vector<std::uint8_t> got;
	Require(ReadWholeFile(JoinPath(client, "Content/data/big.bin"), got) && got == big, "big.bin por HTTP");
	Require(ReadWholeFile(JoinPath(client, "Content/a/small.ozj"), got) && got == chica, "small.ozj por HTTP");
	Require(res.downloaded == plan.downloadBytes && server.cutNext == 0, "hubo corte y no se volvió a bajar lo que ya había llegado");
	k.Wipe();
}

// Copia un canal armado por mu-patch a la carpeta del "Release", con los
// nombres de los assets de un Release del proyecto.
void PublishLikeRelease(const std::string& out, const std::string& channel, const std::string& rel)
{
	std::vector<std::string> names;
	std::string err;
	Require(ListFiles(out, names, &err), "listar canal");
	std::string slug = channel;
	for (char& c : slug)
		if (c == '/') c = '-';
	for (const std::string& n : names)
	{
		std::string to = n;
		if (n == "manifest.json") to = slug + ".manifest.json";
		if (n == "manifest.json.sig") to = slug + ".manifest.json.sig";
		Require(CopyFileTo(JoinPath(out, n), JoinPath(rel, to)), "copiar asset");
	}
}

void TestLauncherConfig(const std::string& dir)
{
	const std::string path = JoinPath(dir, "launcher.toml");
	const std::string text =
		"# comentario\n"
		"Music = false\n"
		"HD = true   # con comentario\n"
		"Connections = 8\n"
		"SpeedLimitKB = 100\n"
		"BaseSource = \"dueño/repo\"\n"
		"Connections = 99\n"
		"esto no es una línea\n";
	Require(WriteFileAtomic(path, text.data(), text.size()), "escribir launcher.toml");
	Launcher::Config c = Launcher::CompiledConfig();
	std::vector<std::string> warnings;
	Launcher::ApplyLauncherToml(path, c, &warnings);
	Require(!c.music && c.hd && c.connections == 8 && c.speedLimit == 100 * 1024, "launcher.toml: valores");
	Require(c.baseRepo == "dueño/repo" && c.baseUrl.empty() && c.overrides.size() == 1, "launcher.toml: origen de la base, con aviso");
	Require(warnings.size() == 2, "launcher.toml: avisa la línea rota y el valor fuera de rango");
	Launcher::Config d = Launcher::CompiledConfig();
	Launcher::ApplyLauncherToml(JoinPath(dir, "no-existe.toml"), d, &warnings);
	Require(d.music && !d.hd && d.connections == 4, "launcher.toml que falta: valores por defecto");
}

void TestLauncherFlow(TestServer& server, const std::string& dir, const std::string& www)
{
	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave flow");
	const std::string key = JoinPath(dir, "flow.key");
	Require(SaveKeyFile(key, k), "guardar clave flow");
	const std::string rel = JoinPath(www, "flow-rel");

	const std::string inBase = JoinPath(dir, "flow-in-base"), inMusic = JoinPath(dir, "flow-in-music");
	const auto data = Random(300000, 7), song = Random(200000, 8);
	Write(JoinPath(inBase, "Content/data/b.bin"), data);
	Write(JoinPath(inMusic, "Content/audio/music/s.mp3"), song);
	// La música es opcional, como en el Release (.github/scripts/client_channels.py).
	auto build = [&](const std::string& channel, const std::string& in, std::uint64_t sequence) {
		MuPatch::BuildOptions o;
		o.channel = channel;
		o.version = "0.97.50";
		o.sequence = sequence;
		o.in = in;
		o.out = JoinPath(dir, "flow-out-" + std::to_string(channel.size()) + "-" + std::to_string(sequence));
		o.keyPath = key;
		o.level = 3;
		o.threads = 2;
		if (channel == "mu-linux/music") o.optional = { "**" };
		Require(MuPatch::BuildPatch(o, &err), ("flow: build " + channel + ": " + err).c_str());
		PublishLikeRelease(o.out, channel, rel);
	};
	build("mu-linux/base", inBase, 1);
	build("mu-linux/music", inMusic, 1);

	Launcher::Config c;
	c.title = "prueba";
	c.baseUrl = server.Url("/flow-rel");
	c.baseTag = "0.97.50";
	c.baseKey = ToHex(k.publicKey, kKeySize);
	c.gameExe = "main.exe";
	const std::string root = JoinPath(dir, "flow-client");
	Write(JoinPath(root, "main.exe"), Random(100, 9));

	auto run = [&](const Launcher::Config& cfg, const std::string& r) {
		Launcher::UpdateFlow flow(cfg, r, std::make_unique<Launcher::AnyTransport>(std::make_unique<HttpTransport>()));
		flow.Start();
		flow.Wait();
		return flow.Snapshot();
	};

	Launcher::Status st = run(c, root);
	std::vector<std::uint8_t> got;
	Require(st.phase == Launcher::Phase::Ready && st.canPlay, ("flow: instala base y música: " + st.message).c_str());
	Require(ReadWholeFile(JoinPath(root, "Content/data/b.bin"), got) && got == data &&
	            ReadWholeFile(JoinPath(root, "Content/audio/music/s.mp3"), got) && got == song, "flow: archivos instalados");
	Require(FileExists(JoinPath(Launcher::UserDirFor(root), "Cache/install.json")) &&
	            FileExists(JoinPath(Launcher::UserDirFor(root), "Cache/sequences.json")), "flow: índice y secuencias");

	st = run(c, root);
	Require(st.phase == Launcher::Phase::Ready && st.message.find("al día") != std::string::npos, "flow: segunda vez, al día");

	// Sin red (o un Release que no está) con algo instalado: se juega igual.
	Launcher::Config off = c;
	off.baseUrl = server.Url("/no-existe");
	st = run(off, root);
	Require(st.phase == Launcher::Phase::Offline && st.canPlay, "flow: sin conexión, juega con lo instalado");
	// Otra versión de la base que la publicada: no se acepta.
	Launcher::Config tag = c;
	tag.baseTag = "0.97.49";
	st = run(tag, root);
	Require(st.phase == Launcher::Phase::Offline, "flow: tag distinto al del manifiesto, no se acepta");
	// Instalación vacía y sin red: no se puede jugar.
	st = run(off, JoinPath(dir, "flow-vacio"));
	Require(st.phase == Launcher::Phase::Failed && !st.canPlay, "flow: sin nada instalado y sin red, no se juega");
	// El cliente descomprimido a mano (sin índice del launcher) y sin red: se
	// juega igual.
	const std::string byHand = JoinPath(dir, "flow-a-mano");
	Write(JoinPath(byHand, "main.exe"), Random(100, 11));
	st = run(off, byHand);
	Require(st.phase == Launcher::Phase::Offline && st.canPlay, "flow: cliente a mano y sin red, se juega");
	// Clave rota.
	Launcher::Config badKey = c;
	badKey.baseKey = "xyz";
	st = run(badKey, root);
	Require(st.phase == Launcher::Phase::Failed, "flow: clave de la base inválida");
	// Música apagada: no se baja, pero lo ya instalado queda.
	Launcher::Config noMusic = c;
	noMusic.music = false;
	st = run(noMusic, root);
	Require(st.phase == Launcher::Phase::Ready && FileExists(JoinPath(root, "Content/audio/music/s.mp3")), "flow: sin música no borra la instalada");

	// ── Jugar mientras baja ───────────────────────────────────────────────
	// Juego cerrado: lo obligatorio primero, ya se puede jugar, después la
	// música; el cliente ve la revisión en content.rev.
	const std::string play = JoinPath(dir, "flow-play");
	Write(JoinPath(play, "main.exe"), Random(100, 19));
	{
		Launcher::UpdateFlow flow(c, play, std::make_unique<Launcher::AnyTransport>(std::make_unique<HttpTransport>()));
		flow.Start();
		bool playableBeforeEnd = false;
		while (!flow.Finished())
		{
			const Launcher::Status s = flow.Snapshot();
			if (s.background && s.canPlay && !FileExists(JoinPath(play, "Content/audio/music/s.mp3"))) playableBeforeEnd = true;
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		flow.Wait();
		st = flow.Snapshot();
		Require(st.phase == Launcher::Phase::Ready && st.background && FileExists(JoinPath(play, "Content/audio/music/s.mp3")),
		        ("jugar mientras baja: instala todo: " + st.message).c_str());
		(void)playableBeforeEnd;   // depende de la velocidad: sólo informativo
		std::string rev;
		Require(ReadWholeFile(JoinPath(Launcher::UserDirFor(play), "Cache/content.rev"), rev) && !rev.empty() && rev[0] >= '1' && rev[0] <= '9',
		        "jugar mientras baja: escribe content.rev");
	}

	// Juego abierto: la música nueva se agrega, la que cambió espera al cierre.
	const auto song2 = Random(200000, 28), song3 = Random(150000, 29);
	Write(JoinPath(inMusic, "Content/audio/music/s.mp3"), song2);
	Write(JoinPath(inMusic, "Content/audio/music/t.mp3"), song3);
	build("mu-linux/music", inMusic, 2);
	const std::string lockPath = JoinPath(Launcher::UserDirFor(play), "Cache/running.lock");
	Require(RunningLock::Acquire(lockPath) && RunningLock::IsHeld(lockPath), "lock del cliente");
	{
		Launcher::UpdateFlow flow(c, play, std::make_unique<Launcher::AnyTransport>(std::make_unique<HttpTransport>()));
		flow.Start();
		Launcher::Status s;
		for (int i = 0; i < 3000 && !flow.Finished(); ++i)
		{
			s = flow.Snapshot();
			if (s.waitingForClose) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		s = flow.Snapshot();
		Require(s.playing && s.waitingForClose == 1 && s.canPlay, ("juego abierto: el archivo que cambió espera al cierre: " + s.message).c_str());
		Require(ReadWholeFile(JoinPath(play, "Content/audio/music/t.mp3"), got) && got == song3, "juego abierto: lo nuevo se agrega");
		Require(ReadWholeFile(JoinPath(play, "Content/audio/music/s.mp3"), got) && got == song, "juego abierto: lo que existe no se pisa");
		flow.Cancel();
		flow.Wait();
		Require(flow.Snapshot().phase == Launcher::Phase::Ready, "juego abierto: cerrar el launcher deja lo demás para la próxima");
	}
	k.Wipe();
}

void TestIniDocument()
{
	// Comentarios, mayúsculas, claves desconocidas y CRLF se conservan.
	const std::string text =
		"[Window]\r\n"
		"WindowMode=1\r\n"
		"resolution = 3 \r\n"
		"ClaveDeUnMod=x\r\n"
		"\r\n"
		"; comentario de [Font]\r\n"
		"[Font]\r\n"
		"FontName=Arial\r\n";
	Launcher::IniDocument d;
	d.Parse(text);
	std::string v;
	Require(d.Get("window", "RESOLUTION", v) && v == "3", "ini: sin mayúsculas y sin espacios");
	Require(!d.Set("Window", "Resolution", "3"), "ini: mismo valor no cambia nada");
	Require(d.Text() == text, "ini: sin cambios, el texto queda igual");
	Require(d.Set("Window", "Resolution", "7"), "ini: cambiar un valor");
	Require(d.Text().find("resolution = 7 \r\n") != std::string::npos, "ini: conserva la clave y los espacios");
	Require(d.Set("Window", "VSync", "1"), "ini: clave nueva");
	const std::string t = d.Text();
	Require(t.find("ClaveDeUnMod=x\r\nVSync=1\r\n\r\n; comentario de [Font]") != std::string::npos,
	        "ini: la clave nueva va al final de su sección, antes del comentario de la siguiente");
	Require(d.Set("MiniMap", "Alpha", "5") && d.Text().find("FontName=Arial\r\n[MiniMap]\r\nAlpha=5\r\n") != std::string::npos,
	        "ini: sección nueva al final");
	Require(d.Text().find("\n") == d.Text().find("\r\n") + 1, "ini: conserva CRLF");

	// .toml: claves en la raíz, antes de la primera sección.
	Launcher::IniDocument toml(" = ");
	toml.Parse("# preferencias\nMusic = true\n");
	Require(toml.Set("", "HD", "true") && toml.Text() == "# preferencias\nMusic = true\nHD = true\n", "toml: clave nueva en la raíz");
	Require(toml.Set("", "Music", "false") && toml.Text() == "# preferencias\nMusic = false\nHD = true\n", "toml: cambiar un valor");
	Launcher::IniDocument empty(" = ");
	empty.Parse("");
	Require(empty.Set("", "HD", "true") && empty.Text() == "HD = true\n", "toml: archivo vacío");
	Launcher::IniDocument noEol;
	noEol.Parse("[A]\nx=1");
	Require(noEol.Set("A", "x", "2") && noEol.Text() == "[A]\nx=2", "ini: sin salto al final, sigue sin salto");
}

void TestOptions(const std::string& dir)
{
	const std::string user = JoinPath(dir, "opt-user");
	const std::string ini = JoinPath(user, "Config.ini"), tmpl = JoinPath(dir, "opt-plantilla.ini"), toml = JoinPath(user, "launcher.toml");
	const std::string plantilla =
		"[Window]\n"
		"WindowMode=1\n"
		"Resolution=3\n"
		"Sharpness=9\n"
		"; nota\n"
		"[Sound]\n"
		"SoundLevel=8\n";
	Require(WriteFileAtomic(tmpl, plantilla.data(), plantilla.size()), "escribir plantilla");

	Launcher::OptionsEditor ed;
	Require(ed.Load(ini, tmpl, toml), "opciones: cargar desde la plantilla");
	auto find = [&](const char* key, std::size_t& t, std::size_t& r) {
		for (t = 0; t < ed.Tabs().size(); ++t)
			for (r = 0; r < ed.Tabs()[t].rows.size(); ++r)
				if (ed.Tabs()[t].rows[r].key == key) return true;
		return false;
	};
	std::size_t t = 0, r = 0;
	Require(find("Resolution", t, r) && ed.Tabs()[t].rows[r].choices[static_cast<std::size_t>(ed.Current(t, r))].label == "1280x1024",
	        "opciones: lee la resolución");
	Require(find("Sharpness", t, r) && ed.Current(t, r) == 0, "opciones: valor fuera de rango se muestra como el default");
	Require(!ed.Dirty(), "opciones: sin cambios");
	Require(find("Resolution", t, r), "fila resolución");
	ed.Cycle(t, r, +1);
	Require(ed.Dirty(), "opciones: hay cambios");
	ed.Cycle(t, r, -1);
	Require(!ed.Dirty(), "opciones: volver al valor guardado no es un cambio");
	ed.Cycle(t, r, +1);
	bool dl = true;
	Require(ed.Save(&dl) && !dl, "opciones: guardar");
	std::string out;
	Require(ReadWholeFile(ini, out), "opciones: escribe User/Config.ini");
	Require(out == "[Window]\nWindowMode=1\nResolution=4\nSharpness=9\n; nota\n[Sound]\nSoundLevel=8\n",
	        "opciones: sólo cambia la resolución; el valor fuera de rango no se toca");
	Require(!FileExists(toml), "opciones: no toca launcher.toml si no cambió una descarga");

	// Texturas HD prendidas: también se bajan.
	Launcher::OptionsEditor ed2;
	Require(ed2.Load(ini, tmpl, toml), "opciones: recargar desde User/");
	Require(find("Resolution", t, r) && ed2.Current(t, r) == 4, "opciones: ahora lee User/Config.ini");
	Require(find("HDTextures", t, r), "fila HD");
	ed2.Select(t, r, 1);
	Require(ed2.Save(&dl) && dl, "opciones: HD avisa que cambian las descargas");
	Require(ReadWholeFile(toml, out) && out == "HD = true\n", "opciones: HD también en launcher.toml");
	Require(ReadWholeFile(ini, out) && out.find("[Window]\nWindowMode=1\nResolution=4\nSharpness=9\nHDTextures=1\n") == 0,
	        "opciones: HDTextures en [Window]");

	// Sin HD en launcher.toml, la descarga sigue a HDTextures del juego.
	RemoveFile(toml);
	Launcher::OptionsEditor ed3;
	Require(ed3.Load(ini, tmpl, toml) && find("HD", t, r) && ed3.Current(t, r) == 1, "opciones: la descarga HD sigue a HDTextures");
	Require(Launcher::HDTexturesEnabled(ini) && !Launcher::HDTexturesEnabled(tmpl), "HDTexturesEnabled");

		// La tabla compartida con el cliente arma las filas.
	Require(find("RenderBackend", t, r) && !ed2.Tabs()[t].rows[r].numeric, "opciones: backend por nombre");
	Require(find("DeleteGlow", t, r) && ed2.Tabs()[t].rows[r].section == "Antilag", "opciones: antilag de la tabla");
	Require(find("LangSelection", t, r) && ed2.Tabs()[t].rows[r].choices.size() == 4, "opciones: idiomas");
	Require(ed2.Tabs()[0].name == "General", "opciones: General es la primera pestaña");

	// Usuario guardado: texto libre, sólo letras y números, hasta 10.
	Launcher::OptionsEditor ed4;
	Require(ed4.Load(ini, tmpl, toml) && find("Username", t, r) && ed4.Tabs()[t].rows[r].text, "opciones: fila de usuario");
	Require(ed4.Text(t, r).empty(), "opciones: sin usuario guardado");
	ed4.SetText(t, r, "mi cuenta;123456789");
	Require(ed4.Text(t, r) == "micuenta12", "opciones: usuario filtrado y cortado en 10");
	ed4.SetText(t, r, "");
	Require(!ed4.Dirty(), "opciones: borrar lo escrito no es un cambio");
	ed4.SetText(t, r, "emanuel");
	std::size_t st = 0, sr = 0;
	Require(find("Scale", st, sr), "opciones: fila de tamaño");
	ed4.Select(st, sr, 3);
	Require(ed4.Save(&dl) && !dl, "opciones: usuario y tamaño no cambian las descargas");
	Require(ReadWholeFile(ini, out) && out.find("[User]\nUsername=emanuel\n") != std::string::npos, "opciones: usuario en [User]");
	Require(ReadWholeFile(toml, out) && out.find("Scale = 150\n") != std::string::npos, "opciones: tamaño en launcher.toml");
	Launcher::Config c;
	Launcher::ApplyLauncherToml(toml, c, nullptr);
	Require(c.scale == 150, "launcher.toml: Scale");
	Launcher::OptionsEditor ed5;
	Require(ed5.Load(ini, tmpl, toml) && ed5.Text(t, r) == "emanuel" && ed5.Current(st, sr) == 3, "opciones: relee usuario y tamaño");
}

// Un ejecutable mínimo de la plataforma donde corre la prueba (mu-patch lo
// reconoce como windows-x64 o linux-x64): el launcher filtra los de otra.
std::vector<std::uint8_t> FakePe(unsigned seed)
{
	std::vector<std::uint8_t> d(0x200, 0);
#ifdef _WIN32
	d[0] = 'M';
	d[1] = 'Z';
	d[0x3C] = 0x40;
	std::memcpy(&d[0x40], "PE\0\0", 4);
	d[0x44] = 0x64;
	d[0x45] = 0x86;
#else
	d[0] = 0x7F;
	d[1] = 'E';
	d[2] = 'L';
	d[3] = 'F';
	d[4] = 2;     // 64 bits
	d[5] = 1;     // little endian
	d[18] = 62;   // x86-64
#endif
	const auto tail = Random(64, seed);
	d.insert(d.end(), tail.begin(), tail.end());
	return d;
}

// ConnectServer de prueba: el saludo del canal cifrado (SecureChannel del lado
// server, el mismo del ConnectServer) y F4:14/15 de a `chunk` bytes.
class FakeCs
{
public:
	std::vector<std::uint8_t> blob;
	KeyPair identity;
	bool rotation = false;
	std::uint8_t previous[kKeySize] = {};
	std::uint8_t rotationSig[kSignatureSize] = {};
	std::size_t chunk = 100;
	std::atomic<int> requests{ 0 };
	int port = 0;

	bool Start()
	{
		m_Listen = socket(AF_INET, SOCK_STREAM, 0);
		if (m_Listen == kNoSock) return false;
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (bind(m_Listen, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 || listen(m_Listen, 4) != 0) return false;
		socklen_t len = sizeof(a);
		getsockname(m_Listen, reinterpret_cast<sockaddr*>(&a), &len);
		port = ntohs(a.sin_port);
		m_Thread = std::thread([this] {
			for (;;)
			{
				Sock c = accept(m_Listen, nullptr, nullptr);
				if (m_Stop)
				{
					if (c != kNoSock) CloseSock(c);
					return;
				}
				if (c != kNoSock) Serve(c);
			}
		});
		return true;
	}

	void Stop()
	{
		m_Stop = true;
		Sock s = socket(AF_INET, SOCK_STREAM, 0);
		sockaddr_in a{};
		a.sin_family = AF_INET;
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		a.sin_port = htons(static_cast<unsigned short>(port));
		connect(s, reinterpret_cast<sockaddr*>(&a), sizeof(a));
		CloseSock(s);
		if (m_Thread.joinable()) m_Thread.join();
		CloseSock(m_Listen);
	}

	std::string Address() const { return "127.0.0.1:" + std::to_string(port); }

private:
	void SendSealed(Sock c, SecureChannel& ch, const std::vector<std::uint8_t>& pkt)
	{
		std::vector<std::uint8_t> out(pkt.size() + SecureChannel::Overhead);
		const std::size_t n = ch.Seal(pkt.data(), pkt.size(), out.data());
		SendAll(c, reinterpret_cast<const char*>(out.data()), n);
	}

	void Serve(Sock c)
	{
		SecureChannel ch;
		std::uint8_t hello[SecureChannel::ServerHelloSize];
		const auto rnd = Random(SecureChannel::KeySize, 77 + port);
		ch.BeginServer(hello, rnd.data(), identity.secret, identity.publicKey, rotation ? previous : nullptr,
		               rotation ? rotationSig : nullptr);
		SendAll(c, reinterpret_cast<const char*>(hello), sizeof(hello));
		std::vector<std::uint8_t> plain;
		bool welcomed = false;
		char buf[4096];
		for (;;)
		{
			const int n = recv(c, buf, sizeof(buf), 0);
			if (n <= 0 || !ch.Receive(reinterpret_cast<std::uint8_t*>(buf), static_cast<std::size_t>(n), plain)) break;
			if (ch.IsReady() && !welcomed)
			{
				welcomed = true;
				SendSealed(c, ch, { 0xC1, 0x04, 0x00, 0x01 });
			}
			while (plain.size() >= 8 && plain[0] == 0xC1 && plain[1] == 8)
			{
				++requests;
				const std::uint32_t offset = plain[4] | plain[5] << 8 | plain[6] << 16 | static_cast<std::uint32_t>(plain[7]) << 24;
				plain.erase(plain.begin(), plain.begin() + 8);
				const std::size_t len = offset < blob.size() ? (std::min)(chunk, blob.size() - offset) : 0;
				const std::size_t total = 16 + len;
				std::vector<std::uint8_t> pkt = { 0xC2, static_cast<std::uint8_t>(total >> 8), static_cast<std::uint8_t>(total), 0xF4, 0x15,
				                                  static_cast<std::uint8_t>(blob.empty() ? 4 : 0) };
				for (std::uint32_t v : { static_cast<std::uint32_t>(blob.size()), offset })
					for (int k = 0; k < 4; ++k) pkt.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
				pkt.push_back(static_cast<std::uint8_t>(len));
				pkt.push_back(static_cast<std::uint8_t>(len >> 8));
				pkt.insert(pkt.end(), blob.begin() + offset, blob.begin() + offset + len);
				SendSealed(c, ch, pkt);
			}
		}
		CloseSock(c);
	}

	Sock m_Listen = kNoSock;
	std::atomic<bool> m_Stop{ false };
	std::thread m_Thread;
};

std::string HexOf(const std::uint8_t* p, std::size_t n)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	for (std::size_t i = 0; i < n; ++i)
	{
		s += digits[p[i] >> 4];
		s += digits[p[i] & 15];
	}
	return s;
}

std::vector<std::uint8_t> AnnounceBlob(std::uint64_t sequence, const std::string& news, const KeyPair& key,
                                       std::string* jsonOut = nullptr)
{
	Announce a;
	a.sequence = sequence;
	a.connectServers = { "mu.ejemplo.com:44405" };
	a.patchChannel = "srv/overlay";
	a.patchUrls = { "https://mu.ejemplo.com/updates" };
	a.news = news;
	const std::string json = DumpAnnounce(a);
	std::uint8_t sig[kSignatureSize];
	Sign(kLauncherContext, json.data(), json.size(), key, sig);
	std::vector<std::uint8_t> blob(sig, sig + kSignatureSize);
	blob.insert(blob.end(), json.begin(), json.end());
	if (jsonOut) *jsonOut = json;
	return blob;
}

// Lo que el launcher aprende del server (ServerAnnounce + CsClient): por el
// ConnectServer en pedazos, una identidad ajena no vale, una secuencia vieja
// tampoco, /updates/ como respaldo y la rotación de la identidad.
// Idioma del launcher: cada texto visible tiene su traducción en Lang.cpp.
void TestLanguage()
{
	using namespace Launcher;
	Require(LanguageFromSetting("Eng") == Language::English && LanguageFromSetting("Por") == Language::Portuguese &&
	            LanguageFromSetting("Spn") == Language::Spanish && LanguageFromSetting("") == Language::Spanish,
	        "idioma: LangSelection del juego");

	// Las opciones: pestañas, filas y valores.  Los que son iguales en todos
	// los idiomas no necesitan entrada.
	const std::set<std::string> same = { "Render", "Antialiasing", "Bloom", "Zoom", "Normal", "Original", "Mipmaps",
	                                     "FXAA", "MSAA 4x", "MSAA 4x + FXAA", "English", "Español", "Português",
	                                     "OpenGL", "WebGPU", "DirectX 12", "DirectX 11", "Vulkan" };
	auto needs = [&](const std::string& s) {
		int letters = 0;
		for (char c : s) letters += (c >= 'a' && c <= 'z') ? 1 : 0;
		return letters >= 2 && s.find("KB/s") == std::string::npos && s.find("MB/s") == std::string::npos &&
		       !same.count(s);
	};
	Launcher::OptionsEditor ed;
	for (const auto& tab : ed.Tabs())
	{
		Require(HasTranslation(tab.name), ("idioma: falta la pestaña " + tab.name).c_str());
		for (const auto& row : tab.rows)
		{
			Require(!needs(row.label) || HasTranslation(row.label), ("idioma: falta la opción " + row.label).c_str());
			for (const auto& c : row.choices)
				Require(!needs(c.label) || HasTranslation(c.label), ("idioma: falta el valor " + c.label).c_str());
		}
	}

	// Los textos que el código pasa por Tr y Fmt (y los de un ternario que sigue
	// en la línea de abajo).
	for (const char* file : { "main.cpp", "UpdateFlow.cpp", "CsClient.cpp", "ServerAnnounce.cpp", "LauncherConfig.cpp" })
	{
		std::ifstream in(std::string(MU_LAUNCHER_SOURCE_DIR) + "/" + file);
		Require(in.good(), (std::string("idioma: no se pudo leer ") + file).c_str());
		std::string line;
		bool continued = false;
		while (std::getline(in, line))
		{
			const std::size_t first = line.find_first_not_of(" \t");
			const bool translated = line.find("Tr(") != std::string::npos || line.find("Fmt(") != std::string::npos;
			if (!translated && !(continued && first != std::string::npos && line[first] == ':'))
			{
				continued = false;
				continue;
			}
			continued = translated;
			for (std::size_t a = line.find('"'); a != std::string::npos; a = line.find('"', a))
			{
				const std::size_t b = line.find('"', a + 1);
				if (b == std::string::npos) break;
				const std::string lit = line.substr(a + 1, b - a - 1);
				int letters = 0;
				for (char c : lit) letters += std::isalpha(static_cast<unsigned char>(c)) ? 1 : 0;
				if (letters >= 3 && lit.find(".h") == std::string::npos)
					Require(HasTranslation(lit), (std::string("idioma: ") + file + " sin traducción: " + lit).c_str());
				a = b + 1;
			}
		}
	}

	SetLanguage(Language::English);
	Require(Fmt("Bajando {0} de {1} archivos ({2} de {3})...", { "3", "10", "1.0 MB", "4.0 MB" }) ==
	            "Downloading 3 of 10 files (1.0 MB of 4.0 MB)...",
	        "idioma: Fmt en inglés");
	Require(std::string(Tr("texto que no está")) == "texto que no está", "idioma: lo que no está sale en castellano");
	SetLanguage(Language::Portuguese);
	Require(Tr(std::string("Jugar")) == "Jogar", "idioma: portugués");
	SetLanguage(Language::Spanish);
	Require(std::string(Tr("Jugar")) == "Jugar", "idioma: castellano");
}

void TestServerAnnounce(TestServer& http, const std::string& dir, const std::string& www)
{
	KeyPair root, other, rotated;
	Require(GenerateKey(root) && GenerateKey(other) && GenerateKey(rotated), "claves del anuncio");
	const std::string cache = JoinPath(dir, "announce-cache");
	Launcher::AnyTransport transport(std::make_unique<HttpTransport>());
	Launcher::SocketCsFetcher fetcher(3000);

	FakeCs cs;
	cs.identity = root;
	cs.blob = AnnounceBlob(2, "Hola desde el server", root);
	Require(cs.Start(), "abrir el ConnectServer de prueba");
	Launcher::AnnounceSeed seed;
	seed.rootKey = HexOf(root.publicKey, kKeySize);
	seed.connectServers = { cs.Address() };

	Launcher::AnnounceResult r = Launcher::ResolveAnnounce(seed, cache, fetcher, transport);
	Require(r.have && r.fresh && r.data.sequence == 2 && r.data.news == "Hola desde el server", "anuncio: por el ConnectServer");
	Require(cs.requests.load() > 1, "anuncio: de a pedazos");
	Require(r.data.patchChannel == "srv/overlay" && r.data.patchUrls.size() == 1, "anuncio: canal y URL de los parches");
	Require(std::memcmp(r.identity, root.publicKey, kKeySize) == 0, "anuncio: la identidad vigente es la raíz");
	Require(FileExists(JoinPath(cache, "launcher.json")), "anuncio: se guarda");

	// Una secuencia menor no reemplaza a la guardada.
	cs.blob = AnnounceBlob(1, "vieja", root);
	r = Launcher::ResolveAnnounce(seed, cache, fetcher, transport);
	Require(r.have && !r.fresh && r.data.sequence == 2 && !r.notes.empty(), "anuncio: una secuencia vieja no vale");

	// Otro server con otra identidad: no se le cree; vale /updates/.
	cs.identity = other;
	cs.blob = AnnounceBlob(9, "impostor", other);
	std::string json;
	const std::vector<std::uint8_t> good = AnnounceBlob(3, "por http", root, &json);
	MakeDirs(JoinPath(www, "ann"));
	Write(JoinPath(www, "ann/launcher.json"), std::vector<std::uint8_t>(json.begin(), json.end()));
	const std::string sigFile = FormatSignatureFile(good.data());
	Write(JoinPath(www, "ann/launcher.json.sig"), std::vector<std::uint8_t>(sigFile.begin(), sigFile.end()));
	seed.urls = { http.Url("/ann") };
	r = Launcher::ResolveAnnounce(seed, cache, fetcher, transport);
	Require(r.have && r.fresh && r.data.sequence == 3 && r.data.news == "por http", "anuncio: identidad ajena rechazada, vale /updates/");

	// Rotación: el server presenta una clave nueva firmada por la anterior.
	cs.identity = rotated;
	cs.rotation = true;
	std::memcpy(cs.previous, root.publicKey, kKeySize);
	Sign("MuIdentityRotation1", rotated.publicKey, kKeySize, root, cs.rotationSig);
	cs.blob = AnnounceBlob(4, "clave nueva", rotated);
	seed.urls.clear();
	r = Launcher::ResolveAnnounce(seed, cache, fetcher, transport);
	Require(r.have && r.fresh && r.data.sequence == 4, "anuncio: después de rotar la identidad");
	Require(std::memcmp(r.identity, rotated.publicKey, kKeySize) == 0, "anuncio: la identidad vigente es la nueva");
	cs.Stop();

	// Sin ConnectServer: queda el guardado, verificado con la clave nueva.
	r = Launcher::ResolveAnnounce(seed, cache, fetcher, transport);
	Require(r.have && !r.fresh && r.data.sequence == 4 && std::memcmp(r.identity, rotated.publicKey, kKeySize) == 0,
	        "anuncio: sin server queda el guardado");
	root.Wipe();
	other.Wipe();
	rotated.Wipe();
}

void TestSelfUpdateMarks(const std::string& dir)
{
	const std::string exe = JoinPath(dir, "self/Launcher/launcher.exe");
	const auto vNew = Random(100, 41), vOld = Random(100, 42);
	Write(exe, vNew);
	Write(exe + ".old", vOld);
	const Launcher::SelfPaths p = Launcher::SelfPathsFor(exe, JoinPath(dir, "self/User/Cache"));
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Normal, "self: sin marca, arranque normal");

	// Arranca bien: confirma y limpia.
	Require(Launcher::MarkPending(p), "self: marca");
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Confirming, "self: primer arranque del nuevo");
	Launcher::ConfirmHealthy(p);
	Require(!FileExists(p.pending) && !FileExists(p.old), "self: confirmado, sin marca ni .old");

	// No arranca: el siguiente inicio vuelve al anterior.
	Write(exe + ".old", vOld);
	Require(Launcher::MarkPending(p), "self: marca 2");
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Confirming, "self: el nuevo toma la marca");
	// ... y se cae antes de confirmar.
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::RolledBack, "self: marca tomada, vuelta atrás");
	std::vector<std::uint8_t> got;
	Require(ReadWholeFile(exe, got) && got == vOld && ReadWholeFile(p.bad, got) && got == vNew && !FileExists(p.pending),
	        "self: restaurado el anterior, el nuevo queda como .bad");
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Normal, "self: después de volver, normal");
}

void TestSelfUpdateFlow(TestServer& server, const std::string& dir, const std::string& www)
{
	std::string err;
	KeyPair k;
	Require(GenerateKey(k), "clave self");
	const std::string key = JoinPath(dir, "self.key");
	Require(SaveKeyFile(key, k), "guardar clave self");

	// Overlay con el launcher nuevo y un archivo del admin.
	const std::string in = JoinPath(dir, "self-ov-in");
	const auto newLauncher = FakePe(51), adminFile = Random(5000, 52);
	Write(JoinPath(in, "Launcher/launcher.exe"), newLauncher);
	Write(JoinPath(in, "Content/admin/a.bin"), adminFile);
	MuPatch::BuildOptions o;
	o.channel = "prueba/overlay";
	o.version = "1.0.0";
	o.base = "0.97.50";
	o.in = in;
	o.out = JoinPath(www, "self-ov");
	o.keyPath = key;
	o.level = 3;
	o.threads = 2;
	Require(MuPatch::BuildPatch(o, &err), ("self: build overlay: " + err).c_str());

	const std::string root = JoinPath(dir, "self-client");
	const auto oldLauncher = FakePe(53);
	Write(JoinPath(root, "Launcher/launcher.exe"), oldLauncher);
	Write(JoinPath(root, "main.exe"), Random(100, 54));

	Launcher::Config c;
	c.overlayUrl = server.Url("/self-ov");
	c.overlayChannel = "prueba/overlay";
	c.overlayKey = ToHex(k.publicKey, kKeySize);
	c.gameExe = "main.exe";
	c.selfPath = "Launcher/launcher.exe";
	auto run = [&](const Launcher::Config& cfg) {
		Launcher::UpdateFlow flow(cfg, root, std::make_unique<Launcher::AnyTransport>(std::make_unique<HttpTransport>()));
		flow.Start();
		flow.Wait();
		return flow.Snapshot();
	};

	// Primero sólo el launcher, y pide reiniciar.
	Launcher::Status st = run(c);
	std::vector<std::uint8_t> got;
	Require(st.phase == Launcher::Phase::Restart, ("self: el launcher nuevo va primero: " + st.message).c_str());
	Require(ReadWholeFile(JoinPath(root, "Launcher/launcher.exe"), got) && got == newLauncher &&
	            ReadWholeFile(JoinPath(root, "Launcher/launcher.exe.old"), got) && got == oldLauncher,
	        "self: el nuevo en su lugar y el anterior como .old");
	Require(!FileExists(JoinPath(root, "Content/admin/a.bin")), "self: el resto espera al launcher nuevo");
	const Launcher::SelfPaths p = Launcher::SelfPathsFor(JoinPath(root, "Launcher/launcher.exe"),
	                                                     JoinPath(Launcher::UserDirFor(root), "Cache"));
	Require(FileExists(p.pending), "self: deja la marca para confirmar");

	// El nuevo arranca, confirma y procesa el resto.
	Require(Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Confirming, "self: el nuevo toma la marca");
	st = run(c);
	Require(st.phase == Launcher::Phase::Ready && ReadWholeFile(JoinPath(root, "Content/admin/a.bin"), got) && got == adminFile,
	        ("self: después del reinicio, el resto: " + st.message).c_str());
	Launcher::ConfirmHealthy(p);
	Require(!FileExists(JoinPath(root, "Launcher/launcher.exe.old")), "self: confirmado, se borra el .old");

	// La versión nueva se cae antes de confirmar: el siguiente arranque vuelve a
	// la anterior y el flujo ya no la reinstala (si no, entraría en un bucle).
	Write(JoinPath(root, "Launcher/launcher.exe.old"), oldLauncher);
	Require(Launcher::MarkPending(p) && Launcher::CheckAtStartup(p) == Launcher::StartupCheck::Confirming &&
	            Launcher::CheckAtStartup(p) == Launcher::StartupCheck::RolledBack, "self: vuelta atrás");
	Digest blocked{};
	Require(Launcher::BlockedVersion(p, blocked) && blocked == HashBytes(newLauncher.data(), newLauncher.size()),
	        "self: anota la versión que no arrancó");
	st = run(c);
	Require(st.phase == Launcher::Phase::Ready && ReadWholeFile(JoinPath(root, "Launcher/launcher.exe"), got) && got == oldLauncher,
	        ("self: no reinstala la versión bloqueada: " + st.message).c_str());
	bool warned = false;
	for (const std::string& n : st.notices) warned |= n.find("no arranca") != std::string::npos;
	Require(warned, "self: avisa que se sigue con la actual");

	// Un launcher fuera de la carpeta del cliente no se actualiza a sí mismo: el
	// de Launcher/ se instala como un archivo más.
	const std::string root2 = JoinPath(dir, "self-client2");
	Write(JoinPath(root2, "main.exe"), Random(100, 55));
	Launcher::Config c2 = c;
	c2.selfPath.clear();
	Launcher::UpdateFlow flow2(c2, root2, std::make_unique<Launcher::AnyTransport>(std::make_unique<HttpTransport>()));
	flow2.Start();
	flow2.Wait();
	st = flow2.Snapshot();
	Require(st.phase == Launcher::Phase::Ready && FileExists(JoinPath(root2, "Launcher/launcher.exe")) &&
	            FileExists(JoinPath(root2, "Content/admin/a.bin")), "self: sin selfPath, todo junto y sin reiniciar");
	k.Wipe();
}

// Con MU_TEST_ONLINE=1: HTTPS real contra un asset de GitHub (redirige a otro
// host), para probar el TLS del sistema.  El CI no lo corre.
void TestOnline()
{
	if (!std::getenv("MU_TEST_ONLINE")) return;
	const std::string url = "https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz";
	HttpTransport http;
	std::string err;
	std::vector<std::uint8_t> head;
	const bool ok = http.Fetch(url, 0, 6, [&](const std::uint8_t* d, std::size_t n) { head.insert(head.end(), d, d + n); return true; }, &err);
	Require(ok && head.size() == 6 && head[0] == 0xFD && head[1] == '7' && head[2] == 'z', ("HTTPS a GitHub: " + err).c_str());
	std::vector<std::uint8_t> mid;
	Require(http.Fetch(url, 1000000, 4096, [&](const std::uint8_t* d, std::size_t n) { mid.insert(mid.end(), d, d + n); return true; }, &err) &&
	            mid.size() == 4096, ("HTTPS por rango: " + err).c_str());
	std::printf("online: HTTPS a GitHub bien\n");
}

} // namespace

int main()
{
	std::setvbuf(stdout, nullptr, _IONBF, 0);
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	namespace fs = std::filesystem;
	const fs::path dir = fs::temp_directory_path() / ("mu_update_http_test_" + std::to_string(std::random_device{}()));
	fs::create_directories(dir);
	const std::string d = dir.generic_string();
	const std::string root = JoinPath(d, "www");
	MakeDirs(root);

	TestServer server;
	if (!server.Start(root))
	{
		std::printf("no se pudo abrir el server de prueba\n");
		return 1;
	}
	TestTransport(server, root);
	TestEndToEnd(server, d, root);
	TestLauncherConfig(d);
	TestIniDocument();
	TestOptions(d);
	TestLanguage();
	TestSelfUpdateMarks(d);
	TestSelfUpdateFlow(server, d, root);
	TestLauncherFlow(server, d, root);
	TestServerAnnounce(server, d, root);
	server.Stop();
	TestOnline();

	std::error_code ec;
	fs::remove_all(dir, ec);
	if (s_Failures)
	{
		std::printf("%d fallas\n", s_Failures);
		return 1;
	}
	std::printf("update_http: todo bien\n");
	return 0;
}
