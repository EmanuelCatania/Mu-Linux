// Transporte HTTP del actualizador (src/update/HttpTransport) y flujo del
// launcher (src/launcher, sin la ventana) contra un server
// HTTP mínimo en 127.0.0.1: rangos, redirecciones, un server que ignora el
// Range, cortes y una instalación de punta a punta que retoma después de un
// corte.  También el armado y la validación de canales (ChannelFetch) por HTTP.

#include "LauncherConfig.h"
#include "PatchBuilder.h"
#include "UpdateFlow.h"

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
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <random>
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
	for (const auto& ch : { std::make_pair(std::string("mu-linux/base"), inBase), std::make_pair(std::string("mu-linux/music"), inMusic) })
	{
		MuPatch::BuildOptions o;
		o.channel = ch.first;
		o.version = "0.97.50";
		o.in = ch.second;
		o.out = JoinPath(dir, "flow-out-" + std::to_string(ch.first.size()));
		o.keyPath = key;
		o.level = 3;
		o.threads = 2;
		Require(MuPatch::BuildPatch(o, &err), ("flow: build " + ch.first + ": " + err).c_str());
		PublishLikeRelease(o.out, ch.first, rel);
	}

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
	TestLauncherFlow(server, d, root);
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
