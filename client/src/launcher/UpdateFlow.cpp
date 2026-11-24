#include "UpdateFlow.h"

#include "update/ChannelFetch.h"
#include "update/FileUtil.h"
#include "update/InstallIndex.h"
#include "update/InstallPlan.h"
#include "update/Installer.h"
#include "update/Signing.h"

#include "SelfUpdate.h"
#include "ServerAnnounce.h"

#include "platform/RunningLock.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace Launcher {

using namespace Update;

namespace {

#ifdef _WIN32
constexpr const char* kPlatform = "windows-x64";
#else
constexpr const char* kPlatform = "linux-x64";
#endif

bool IsLocal(const std::string& url)
{
	return url.compare(0, 7, "file://") == 0 || url.find("://") == std::string::npos;
}

std::string HexKey(const std::uint8_t* p)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	for (std::size_t i = 0; i < kKeySize; ++i)
	{
		s += digits[p[i] >> 4];
		s += digits[p[i] & 15];
	}
	return s;
}

std::string Human(std::uint64_t bytes)
{
	char buf[32];
	if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / 1048576.0);
	else std::snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
	return buf;
}

} // namespace

AnyTransport::AnyTransport(std::unique_ptr<Transport> http) : m_Http(std::move(http)) {}

Transport& AnyTransport::Pick(const std::string& url)
{
	if (IsLocal(url) || !m_Http) return m_File;
	return *m_Http;
}

bool AnyTransport::Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length, const DataSink& sink,
                         std::string* error)
{
	return Pick(url).Fetch(url, offset, length, sink, error);
}

bool AnyTransport::Get(const std::string& url, std::uint64_t maxSize, std::vector<std::uint8_t>& out, std::string* error)
{
	return Pick(url).Get(url, maxSize, out, error);
}

UpdateFlow::UpdateFlow(Config config, std::string root, std::unique_ptr<Transport> transport)
	: m_Config(std::move(config)), m_Root(std::move(root)), m_Transport(std::move(transport)), m_Limiter(m_Config.speedLimit)
{
	m_LockPath = JoinPath(JoinPath(UserDirFor(m_Root), "Cache"), "running.lock");
	m_MaxConnections = std::max(1, m_Config.connections);
	for (const std::string& o : m_Config.overrides) m_Status.notices.push_back("Configuración de prueba: " + o);
}

UpdateFlow::~UpdateFlow()
{
	Cancel();
	Wait();
}

void UpdateFlow::Start()
{
	m_Thread = std::thread([this] { Run(); });
}

Status UpdateFlow::Snapshot() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Status;
}

void UpdateFlow::Cancel()
{
	m_Cancel = true;
	m_Limiter.Wake();
}

void UpdateFlow::Wait()
{
	if (m_Thread.joinable()) m_Thread.join();
}

namespace {
std::int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

void UpdateFlow::SetPlaying()
{
	m_PlayingSince = NowMs();
	PollGame(true);
}

void UpdateFlow::SetPaused(bool paused)
{
	m_Paused = paused;
	m_Limiter.SetPaused(paused);
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Status.paused = paused;
}

void UpdateFlow::PollGame(bool force)
{
	std::lock_guard<std::mutex> poll(m_PollMutex);
	const auto now = std::chrono::steady_clock::now();
	if (!force && now - m_LastPoll < std::chrono::milliseconds(500)) return;
	m_LastPoll = now;
	// Recién arrancado, el cliente tarda en tomar su lock: se lo da por abierto.
	const std::int64_t since = m_PlayingSince.load();
	const bool running = RunningLock::IsHeld(m_LockPath) || (since != 0 && NowMs() - since < 15000);
	const bool was = m_GameRunning.exchange(running);
	if (running != was || force)
	{
		std::uint64_t rate = m_Config.speedLimit;
		if (running && m_Config.playingSpeedLimit && (!rate || m_Config.playingSpeedLimit < rate)) rate = m_Config.playingSpeedLimit;
		m_Limiter.SetRate(rate);
		m_MaxConnections = running ? 1 : std::max(1, m_Config.connections);
	}
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Status.playing = running;
}

void UpdateFlow::Set(Phase phase, const std::string& message)
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	m_Status.phase = phase;
	m_Status.message = message;
}

void UpdateFlow::Run()
{
	const std::string user = UserDirFor(m_Root);
	const std::string cache = JoinPath(user, "Cache");
	const std::string indexPath = JoinPath(cache, "install.json");
	const std::string seqPath = JoinPath(cache, "sequences.json");
	const bool gameThere = FileExists(JoinPath(m_Root, m_Config.gameExe));

	InstallIndex index;
	std::string err;
	if (!LoadIndex(indexPath, index, &err)) index = InstallIndex();   // roto: se verifica todo
	const bool installed = gameThere && !index.files.empty();

	auto finish = [&](Phase phase, const std::string& message, bool canPlay) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.phase = phase;
		m_Status.message = message;
		m_Status.canPlay = canPlay;
		m_Finished = true;
	};
	auto unreachable = [&](const std::string& why) {
		if (installed)
			finish(Phase::Offline, "No se pudo buscar actualizaciones (" + why + "). Podés jugar con lo instalado.", true);
		else
			finish(Phase::Failed, why, false);
	};

	SequenceStore sequences;
	if (!sequences.Load(seqPath, &err)) sequences = SequenceStore();

	// ── Canales ───────────────────────────────────────────────────────────
	struct Wanted
	{
		ChannelSpec spec;
		bool optional = false;
	};
	std::vector<Wanted> wanted;
	auto parseKey = [&](const std::string& hex, std::uint8_t out[kKeySize]) { return ParsePublicKey(hex, out); };

	// ── Configuración del server ──────────────────────────────────────────
	std::vector<std::string> overlayUrls;
	if (!m_Config.overlayKey.empty() && (!m_Config.connectServers.empty() || !m_Config.overlayUrl.empty() ||
	                                     FileExists(JoinPath(cache, "launcher.json"))))
	{
		Set(Phase::Checking, "Buscando la configuración del server...");
		SocketCsFetcher cs(4000, &m_Cancel);
		AnnounceSeed seed;
		seed.rootKey = m_Config.overlayKey;
		seed.connectServers = m_Config.connectServers;
		if (!m_Config.overlayUrl.empty()) seed.urls.push_back(m_Config.overlayUrl);
		const AnnounceResult a = ResolveAnnounce(seed, cache, cs, *m_Transport, &m_Cancel);
		if (m_Cancel) return finish(Phase::Failed, "Cancelado.", installed);
		// La identidad vigente (si rotó, la nueva) verifica el manifiesto del overlay.
		if (a.haveIdentity) m_Config.overlayKey = HexKey(a.identity);
		if (a.have)
		{
			const Announce& n = a.data;
			overlayUrls = n.patchUrls;
			if (!n.patchChannel.empty()) m_Config.overlayChannel = n.patchChannel;
			if (m_Config.overrides.empty())
			{
				if (!n.baseSource.empty())
				{
					const bool isUrl = n.baseSource.find("://") != std::string::npos;
					m_Config.baseUrl = isUrl ? n.baseSource : std::string();
					m_Config.baseRepo = isUrl ? std::string() : n.baseSource;
				}
				if (!n.baseTag.empty()) m_Config.baseTag = n.baseTag;
				if (!n.baseKey.empty()) m_Config.baseKey = n.baseKey;
			}
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Status.news = n.news;
		}
	}
	// La compilada queda como último espejo.
	if (!m_Config.overlayUrl.empty() &&
	    std::find(overlayUrls.begin(), overlayUrls.end(), m_Config.overlayUrl) == overlayUrls.end())
		overlayUrls.push_back(m_Config.overlayUrl);
	if (!m_Config.baseKey.empty())
	{
		std::uint8_t key[kKeySize];
		if (!parseKey(m_Config.baseKey, key))
		{
			finish(Phase::Failed, "La clave de la base no es válida (64 dígitos hex).", false);
			return;
		}
		const std::string url = m_Config.baseUrl.empty() ? GithubReleaseUrl(m_Config.baseRepo, m_Config.baseTag) : m_Config.baseUrl;
		const struct { const char* channel; bool on; bool optional; int priority; } base[] = {
			{ "mu-linux/base", true, false, 0 },
			{ "mu-linux/music", m_Config.music, true, 5 },
			{ "mu-linux/hd", m_Config.hd, true, 10 },
		};
		for (const auto& b : base)
		{
			if (!b.on) continue;
			Wanted w;
			w.spec.channel = b.channel;
			w.spec.urls = { url };
			w.spec.manifestName = ReleaseManifestName(b.channel);
			std::memcpy(w.spec.publicKey, key, kKeySize);
			w.spec.version = m_Config.baseTag;
			w.spec.priority = b.priority;
			w.optional = b.optional;
			wanted.push_back(w);
		}
	}
	// Sin clave de la base el launcher sólo trae los archivos del server: es una
	// configuración válida (el operador reparte el cliente entero) y no se avisa
	// al jugador.  Sin nada para bajar, lo dice la línea de estado.
	if (!overlayUrls.empty())
	{
		Wanted w;
		if (!parseKey(m_Config.overlayKey, w.spec.publicKey) || m_Config.overlayChannel.empty())
		{
			finish(Phase::Failed, "Falta la clave o el canal de los parches del server.", false);
			return;
		}
		w.spec.channel = m_Config.overlayChannel;
		w.spec.urls = overlayUrls;
		w.spec.priority = 20;
		wanted.push_back(w);
	}
	if (wanted.empty())
	{
		finish(gameThere ? Phase::Ready : Phase::Failed,
		       gameThere ? "No hay nada configurado para actualizar." : "No hay de dónde bajar el cliente.", gameThere);
		return;
	}

	Set(Phase::Checking, m_Config.fullVerify ? "Verificando los archivos..." : "Buscando actualizaciones...");
	std::vector<FetchedChannel> fetched(wanted.size());
	std::vector<bool> have(wanted.size(), false);
	for (std::size_t i = 0; i < wanted.size(); ++i)
	{
		if (m_Cancel) return finish(Phase::Failed, "Cancelado.", installed);
		std::string why;
		if (FetchChannel(*m_Transport, wanted[i].spec, sequences, fetched[i], &why))
		{
			have[i] = true;
			continue;
		}
		if (fetched[i].unknownFormat)
			return finish(Phase::Failed, "Hay una versión nueva del launcher que esta no sabe instalar: bajala del sitio del server.", installed);
		if (!wanted[i].optional) return unreachable(why);
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.notices.push_back("No se pudo bajar " + wanted[i].spec.channel + ": " + why);
	}
	sequences.Save(seqPath);

	const Manifest* base = nullptr;
	const Manifest* overlay = nullptr;
	std::vector<ChannelSource> sources;
	for (std::size_t i = 0; i < wanted.size(); ++i)
	{
		if (!have[i]) continue;
		sources.push_back(SourceOf(fetched[i], wanted[i].spec.priority));
		if (wanted[i].spec.channel == "mu-linux/base") base = &fetched[i].manifest;
		if (wanted[i].spec.priority == 20) overlay = &fetched[i].manifest;
	}
	if (base && overlay && !CheckRequires(*base, *overlay, 0, &err))
		return finish(Phase::Failed, err, false);

	// ── Plan e instalación ────────────────────────────────────────────────
	PollGame(true);
	PlanOptions po;
	po.root = m_Root;
	po.platform = kPlatform;
	po.fullVerify = m_Config.fullVerify;
	po.gameRunning = m_GameRunning.load();
	Plan plan;
	if (!BuildPlan(sources, po, index, plan, &err)) return finish(Phase::Failed, err, false);
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.total = plan.downloadBytes;
	}

	// ── El launcher primero ───────────────────────────────────────────────
	bool ownUpdate = false;
	const SelfPaths self = SelfPathsFor(JoinPath(m_Root, m_Config.selfPath), cache);
	Digest blocked{};
	const bool haveBlocked = !m_Config.selfPath.empty() && BlockedVersion(self, blocked);
	for (const PlanFile& f : plan.files)
	{
		if (!f.entry->self || m_Config.selfPath.empty() || f.entry->path != m_Config.selfPath) continue;
		if (haveBlocked && f.entry->hash == blocked)
		{
			// Esa versión ya no arrancó una vez: se sigue con ésta (y no se la
			// instala junto con el resto, que la pisaría sin reiniciar).
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Status.notices.push_back("La versión nueva del launcher no arranca en este equipo; se sigue con la actual.");
			}
			po.includeSelf = false;
			if (!BuildPlan(sources, po, index, plan, &err)) return finish(Phase::Failed, err, false);
			break;
		}
		ownUpdate = true;
	}
	if (ownUpdate)
	{
		Set(Phase::Downloading, "Actualizando el launcher...");
		InstallOptions so;
		so.root = m_Root;
		so.downloadDir = JoinPath(cache, "downloads");
		so.indexPath = indexPath;
		so.transport = m_Transport.get();
		so.limiter = &m_Limiter;
		so.cancel = &m_Cancel;
		so.selfOnly = true;
		InstallResult sr;
		if (!RunPlan(sources, plan, index, so, sr))
			return finish(installed ? Phase::Offline : Phase::Failed,
			              "No se pudo actualizar el launcher: " + (sr.failures.empty() ? std::string("cancelado") : sr.failures.front()),
			              installed);
		if (!MarkPending(SelfPathsFor(JoinPath(m_Root, m_Config.selfPath), cache)))
			return finish(Phase::Failed, "No se pudo escribir en " + cache, installed);
		return finish(Phase::Restart, "Launcher actualizado. Reiniciando...", false);
	}

	const std::size_t heldForGame = plan.heldForGame;
	if (plan.files.empty() && plan.remove.empty() && heldForGame == 0)
	{
		SaveIndex(indexPath, index);
		return finish(Phase::Ready, "El cliente está al día.", FileExists(JoinPath(m_Root, m_Config.gameExe)));
	}
	Set(Phase::Downloading, "Bajando " + std::to_string(plan.files.size()) + " archivos (" + Human(plan.downloadBytes) + ")...");

	InstallOptions io;
	io.root = m_Root;
	io.downloadDir = JoinPath(cache, "downloads");
	io.indexPath = indexPath;
	io.revisionPath = JoinPath(cache, "content.rev");
	io.transport = m_Transport.get();
	io.limiter = &m_Limiter;
	io.connections = std::max(1, m_Config.connections);
	io.maxConnections = &m_MaxConnections;
	io.gameRunning = &m_GameRunning;
	io.cancel = &m_Cancel;
	io.progress = [this](const InstallStatus& s) {
		PollGame();
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.done = s.downloaded;
		m_Status.total = s.downloadTotal;
		m_Status.message = std::string(m_Status.background ? "Ya podés jugar. Bajando " : "Bajando ") +
		                   std::to_string(s.filesDone) + " de " + std::to_string(s.filesTotal) + " archivos (" +
		                   Human(s.downloaded) + " de " + Human(s.downloadTotal) + ")...";
	};

	InstallResult total;
	auto add = [&](const InstallResult& r) {
		total.installed += r.installed;
		total.cancelled = total.cancelled || r.cancelled;
		total.failures.insert(total.failures.end(), r.failures.begin(), r.failures.end());
		total.deferred.insert(total.deferred.end(), r.deferred.begin(), r.deferred.end());
	};
	const bool hasOptional = std::any_of(plan.requests.begin(), plan.requests.end(),
	                                     [](const RangeRequest& r) { return !r.required && !r.self; });
	bool requiredFailed = false;
	if (hasOptional && !m_GameRunning.load())
	{
		// Primero lo necesario para jugar...
		InstallResult res;
		io.part = InstallOptions::Part::Required;
		RunPlan(sources, plan, index, io, res);
		add(res);
		if (total.cancelled) return finish(Phase::Failed, "Cancelado.", installed);
		requiredFailed = !res.failures.empty();
		// ...y ya se puede jugar mientras baja el resto.
		if (!requiredFailed)
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Status.background = true;
			m_Status.canPlay = FileExists(JoinPath(m_Root, m_Config.gameExe));
		}
		io.part = InstallOptions::Part::Optional;
		res = InstallResult();
		RunPlan(sources, plan, index, io, res);
		add(res);
	}
	else
	{
		InstallResult res;
		RunPlan(sources, plan, index, io, res);
		add(res);
		requiredFailed = !total.failures.empty();
	}
	if (total.cancelled) return finish(Phase::Failed, "Cancelado.", installed);

	// Lo que no se pudo pisar con el juego abierto (o el plan dejó afuera por
	// eso): al cerrarlo.
	const std::size_t waiting = total.deferred.size() + heldForGame;
	if (waiting > 0)
	{
		{
			std::lock_guard<std::mutex> lock(m_Mutex);
			m_Status.background = true;
			m_Status.waitingForClose = waiting;
			m_Status.canPlay = FileExists(JoinPath(m_Root, m_Config.gameExe));
			m_Status.message = std::to_string(waiting) + " archivos se instalan cuando cierres el juego.";
		}
		for (;;)
		{
			if (m_Cancel) return finish(Phase::Ready, "Faltan " + std::to_string(waiting) +
			                                              " archivos: se instalan la próxima vez.", true);
			PollGame(true);
			if (!m_GameRunning.load()) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
		}
		Set(Phase::Downloading, "Instalando lo que faltaba...");
		po.gameRunning = false;
		if (!BuildPlan(sources, po, index, plan, &err)) return finish(Phase::Failed, err, false);
		InstallResult res;
		io.part = InstallOptions::Part::All;
		RunPlan(sources, plan, index, io, res);
		total.deferred.clear();
		add(res);
		if (total.cancelled) return finish(Phase::Failed, "Cancelado.", installed);
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.waitingForClose = 0;
	}

	const bool playable = FileExists(JoinPath(m_Root, m_Config.gameExe));
	if (!total.failures.empty())
	{
		const std::string what = "No se pudieron instalar " + std::to_string(total.failures.size()) + " archivos: " + total.failures.front();
		// Si lo que falló es opcional, igual se puede jugar.
		if (!requiredFailed && playable)
		{
			{
				std::lock_guard<std::mutex> lock(m_Mutex);
				m_Status.notices.push_back(what);
			}
			return finish(Phase::Ready, "Actualizado: " + std::to_string(total.installed) + " archivos.", true);
		}
		return finish(Phase::Failed, what, playable && installed);
	}
	finish(Phase::Ready, "Actualizado: " + std::to_string(total.installed) + " archivos.", playable);
}

} // namespace Launcher
