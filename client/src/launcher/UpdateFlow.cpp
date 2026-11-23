#include "UpdateFlow.h"

#include "update/ChannelFetch.h"
#include "update/FileUtil.h"
#include "update/InstallIndex.h"
#include "update/InstallPlan.h"
#include "update/Installer.h"
#include "update/Signing.h"

#include <cstdio>
#include <cstring>

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
	else
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.notices.push_back("Este launcher no tiene la clave de la base: sólo se actualizan los archivos del server.");
	}
	if (!m_Config.overlayUrl.empty())
	{
		Wanted w;
		if (!parseKey(m_Config.overlayKey, w.spec.publicKey) || m_Config.overlayChannel.empty())
		{
			finish(Phase::Failed, "Falta la clave o el canal de los parches del server.", false);
			return;
		}
		w.spec.channel = m_Config.overlayChannel;
		w.spec.urls = { m_Config.overlayUrl };
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
	PlanOptions po;
	po.root = m_Root;
	po.platform = kPlatform;
	po.includeSelf = false;
	po.fullVerify = m_Config.fullVerify;
	Plan plan;
	if (!BuildPlan(sources, po, index, plan, &err)) return finish(Phase::Failed, err, false);
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.newLauncher = plan.selfUpdate;
		m_Status.total = plan.downloadBytes;
	}
	if (plan.files.empty() && plan.remove.empty())
	{
		SaveIndex(indexPath, index);
		return finish(Phase::Ready, "El cliente está al día.", FileExists(JoinPath(m_Root, m_Config.gameExe)));
	}
	Set(Phase::Downloading, "Bajando " + std::to_string(plan.files.size()) + " archivos (" + Human(plan.downloadBytes) + ")...");

	InstallOptions io;
	io.root = m_Root;
	io.downloadDir = JoinPath(cache, "downloads");
	io.indexPath = indexPath;
	io.transport = m_Transport.get();
	io.limiter = &m_Limiter;
	io.connections = m_Config.connections;
	io.cancel = &m_Cancel;
	io.progress = [this](const InstallStatus& s) {
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Status.done = s.downloaded;
		m_Status.total = s.downloadTotal;
		m_Status.message = "Bajando " + std::to_string(s.filesDone) + " de " + std::to_string(s.filesTotal) + " archivos (" +
		                   Human(s.downloaded) + " de " + Human(s.downloadTotal) + ")...";
	};
	InstallResult res;
	const bool ok = RunPlan(sources, plan, index, io, res);
	if (res.cancelled) return finish(Phase::Failed, "Cancelado.", installed);
	const bool playable = FileExists(JoinPath(m_Root, m_Config.gameExe));
	if (!ok)
		return finish(Phase::Failed, "No se pudieron instalar " + std::to_string(res.failures.size()) + " archivos: " +
		                                 (res.failures.empty() ? std::string() : res.failures.front()), playable && installed);
	finish(Phase::Ready, "Actualizado: " + std::to_string(res.installed) + " archivos.", playable);
}

} // namespace Launcher
