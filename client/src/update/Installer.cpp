#include "update/Installer.h"

#include "update/Delta.h"
#include "update/FileUtil.h"
#include "update/Zip.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>

namespace Update {

namespace {

using Clock = std::chrono::steady_clock;

// Hilos para escribir los archivos de un tramo ya bajado.
constexpr std::size_t kInstallThreads = 4;

enum class PieceResult { Ok, Corrupt, DeltaFailed, InstallFailed, Skipped };

struct Run
{
	const std::vector<ChannelSource>& sources;
	const InstallOptions& o;
	InstallIndex& index;
	InstallResult& result;

	std::mutex mutex;           // index, result, status, fallback, log
	InstallStatus status;
	Clock::time_point lastProgress{};
	std::atomic<std::uint64_t> downloaded{ 0 };
	std::size_t sinceSave = 0;
	bool dirty = false;
	std::vector<PlanFile> fallback;   // deltas que no sirvieron

	bool Cancelled() const { return o.cancel && o.cancel->load(); }

	void Log(const std::string& line)
	{
		if (o.log) o.log(line);
	}

	// Con el mutex tomado.
	void Report(bool force)
	{
		if (!o.progress) return;
		const Clock::time_point now = Clock::now();
		if (!force && now - lastProgress < std::chrono::milliseconds(100)) return;
		lastProgress = now;
		status.downloaded = downloaded.load();
		o.progress(status);
	}

	// Con el mutex tomado.
	void SaveLocked()
	{
		if (!dirty || o.indexPath.empty()) return;
		++index.revision;
		if (!SaveIndex(o.indexPath, index)) Log("no se pudo guardar " + o.indexPath);
		dirty = false;
		sinceSave = 0;
	}
};

void Backoff(Run& run, int attempt, std::mt19937& rng)
{
	std::uint64_t delay = run.o.backoffMs;
	for (int i = 0; i < attempt && delay < run.o.backoffMaxMs; ++i) delay *= 2;
	delay = std::min<std::uint64_t>(delay, run.o.backoffMaxMs);
	if (delay) delay += rng() % (delay / 4 + 1);
	const Clock::time_point until = Clock::now() + std::chrono::milliseconds(delay);
	while (Clock::now() < until && !run.Cancelled())
		std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::uint64_t>(delay, 50)));
}

PieceResult InstallPiece(Run& run, const PlanFile& pf, std::size_t fileIndex, const std::string& part,
                         std::uint64_t offset, std::uint64_t packed, std::string* why)
{
	const FileEntry& e = *pf.entry;
	std::vector<std::uint8_t> data;
	if (!ReadFileRange(part, offset, packed, data))
	{
		*why = "no se pudo leer lo bajado";
		return PieceResult::Corrupt;
	}
	const std::string target = JoinPath(run.o.root, e.path);
	std::vector<std::uint8_t> file;
	if (pf.delta)
	{
		std::vector<std::uint8_t> old;
		std::string err;
		if (!ReadWholeFile(target, old) || HashBytes(old.data(), old.size()) != pf.delta->from ||
		    !DeltaApply(old, data.data(), data.size(), e.size, file, &err) || HashBytes(file.data(), file.size()) != e.hash)
		{
			*why = "el delta no dio el archivo esperado";
			return PieceResult::DeltaFailed;
		}
	}
	else
	{
		std::string err;
		if (!ZipDecode(e.method, data.data(), data.size(), e.size, file, &err))
		{
			*why = err;
			return PieceResult::Corrupt;
		}
		if (HashBytes(file.data(), file.size()) != e.hash)
		{
			*why = "el hash no coincide";
			return PieceResult::Corrupt;
		}
	}

	const std::string tmp = JoinPath(run.o.downloadDir, ToHex(e.hash.data(), 8) + "-" + std::to_string(fileIndex) + ".new");
	if (!WriteFileAtomic(tmp, file.data(), file.size()))
	{
		*why = "no se pudo escribir en " + run.o.downloadDir;
		return PieceResult::InstallFailed;
	}
	std::string err;
	// El launcher puede estar corriendo: no se pisa, se renombra a ".old"
	// (Windows lo permite con el archivo abierto) y queda para volver atrás.
	const std::string old = target + ".old";
	const bool swapSelf = e.self && FileExists(target);
	if (swapSelf)
	{
		RemoveFile(old);
		if (!MoveFileOver(target, old, &err))
		{
			RemoveFile(tmp);
			*why = err;
			return PieceResult::InstallFailed;
		}
	}
	if (!MoveFileOver(tmp, target, &err))
	{
		RemoveFile(tmp);
		if (swapSelf) MoveFileOver(old, target, nullptr);
		*why = err;
		return PieceResult::InstallFailed;
	}

	if (e.platform.compare(0, 6, "linux-") == 0) SetExecutable(target);
	std::int64_t mtime = 0;
	FileModTime(target, mtime);
	std::lock_guard<std::mutex> lock(run.mutex);
	IndexEntry& ie = run.index.files[e.path];
	ie.size = e.size;
	ie.mtime = mtime;
	ie.hash = e.hash;
	ie.channel = run.sources[pf.channel].manifest->channel;
	++run.result.installed;
	if (pf.delta) ++run.result.deltasApplied;
	++run.status.filesDone;
	run.status.lastFile = e.path;
	run.dirty = true;
	if (++run.sinceSave >= 64) run.SaveLocked();
	run.Report(true);
	return PieceResult::Ok;
}

void ProcessRequest(Run& run, const std::vector<PlanFile>& files, const RangeRequest& req, std::mt19937& rng)
{
	const ChannelSource& src = run.sources[req.channel];
	const BundleRef& bundle = src.manifest->bundles[req.bundle];
	const std::vector<std::string> urls = BundleUrls(src, req.bundle);
	const std::string part = JoinPath(run.o.downloadDir, ToHex(bundle.hash.data(), 8) + "-" + std::to_string(req.offset) +
	                                                        "-" + std::to_string(req.length) + ".part");

	// char y no bool: los hilos de la instalación escriben posiciones distintas.
	std::vector<char> done(req.pieces.size(), 0);
	if (run.o.selfOnly)
		for (std::size_t i = 0; i < req.pieces.size(); ++i)
			if (!files[req.pieces[i].file].entry->self) done[i] = true;

	std::string lastError = "sin intentos";
	const int attempts = std::max(1, run.o.maxAttempts);
	for (int attempt = 0; attempt < attempts; ++attempt)
	{
		if (run.Cancelled()) return;
		if (std::all_of(done.begin(), done.end(), [](char d) { return d != 0; })) break;
		if (attempt) Backoff(run, attempt - 1, rng);
		if (run.Cancelled()) return;

		std::uint64_t have = 0;
		if (!FileSize(part, have)) have = 0;
		if (have > req.length)
		{
			RemoveFile(part);
			have = 0;
		}
		if (have < req.length)
		{
			if (!MakeDirs(run.o.downloadDir))
			{
				lastError = "no se pudo crear " + run.o.downloadDir;
				break;
			}
			FILE* f = OpenFile(part, have ? "ab" : "wb");
			if (!f)
			{
				lastError = "no se pudo escribir " + part;
				break;
			}
			const std::string& url = urls[static_cast<std::size_t>(attempt) % urls.size()];
			bool diskOk = true;
			const DataSink sink = [&](const std::uint8_t* data, std::size_t size) {
				if (run.o.limiter && !run.o.limiter->Acquire(size, run.o.cancel)) return false;
				if (run.Cancelled()) return false;
				if (std::fwrite(data, 1, size, f) != size)
				{
					diskOk = false;
					return false;
				}
				run.downloaded += size;
				std::lock_guard<std::mutex> lock(run.mutex);
				run.result.downloaded += size;
				run.Report(false);
				return true;
			};
			std::string err;
			const bool ok = run.o.transport->Fetch(url, req.offset + have, req.length - have, sink, &err);
			const bool closed = std::fclose(f) == 0;
			if (run.Cancelled()) return;
			if (!ok || !closed || !diskOk)
			{
				lastError = !diskOk || !closed ? "no se pudo escribir " + part : (err.empty() ? "falló " + url : err);
				std::lock_guard<std::mutex> lock(run.mutex);
				run.Log(bundle.name + " [" + std::to_string(req.offset) + "+" + std::to_string(req.length) +
				        "], intento " + std::to_string(attempt + 1) + ": " + lastError);
				continue;
			}
		}

		// Instalar los archivos del tramo en paralelo: con un zip entero son miles
		// de archivos chicos, y escribir cada uno cuesta más que decodificarlo.
		std::atomic<bool> corrupt{ false };
		std::atomic<std::size_t> nextPiece{ 0 };
		std::mutex errorMutex;
		auto installPieces = [&] {
			for (;;)
			{
				const std::size_t i = nextPiece++;
				if (i >= req.pieces.size() || run.Cancelled()) return;
				if (done[i]) continue;
				const Piece& pc = req.pieces[i];
				const PlanFile& pf = files[pc.file];
				std::string why;
				switch (InstallPiece(run, pf, pc.file, part, pc.offset, pc.packed, &why))
				{
				case PieceResult::Ok:
				case PieceResult::Skipped:
					done[i] = 1;
					break;
				case PieceResult::DeltaFailed:
				{
					done[i] = 1;
					std::lock_guard<std::mutex> lock(run.mutex);
					PlanFile full = pf;
					full.delta = nullptr;
					run.fallback.push_back(full);
					++run.result.deltaFallbacks;
					run.Log(pf.entry->path + ": " + why + "; se baja entero");
					break;
				}
				case PieceResult::InstallFailed:
				{
					done[i] = 1;
					std::lock_guard<std::mutex> lock(run.mutex);
					run.result.failures.push_back(pf.entry->path + ": " + why);
					break;
				}
				case PieceResult::Corrupt:
				{
					corrupt = true;
					std::lock_guard<std::mutex> lock(errorMutex);
					lastError = why;
					break;
				}
				}
			}
		};
		const std::size_t workers = std::min<std::size_t>(req.pieces.size(), kInstallThreads);
		std::vector<std::thread> pool;
		for (std::size_t w = 1; w < workers; ++w) pool.emplace_back(installPieces);
		installPieces();
		for (std::thread& t : pool) t.join();
		if (run.Cancelled()) return;
		RemoveFile(part);
		if (!corrupt) return;
		std::lock_guard<std::mutex> lock(run.mutex);
		run.Log(bundle.name + ": lo bajado no coincide con el manifiesto (" + lastError + "); se vuelve a pedir");
	}

	std::lock_guard<std::mutex> lock(run.mutex);
	for (std::size_t i = 0; i < req.pieces.size(); ++i)
		if (!done[i]) run.result.failures.push_back(files[req.pieces[i].file].entry->path + ": " + lastError);
}

void RunRequests(Run& run, const std::vector<PlanFile>& files, const std::vector<RangeRequest>& requests)
{
	std::vector<const RangeRequest*> todo;
	for (const RangeRequest& r : requests)
		if (!run.o.selfOnly || r.self) todo.push_back(&r);
	if (todo.empty()) return;

	std::atomic<std::size_t> next{ 0 };
	auto worker = [&](unsigned seed) {
		std::mt19937 rng(seed);
		for (;;)
		{
			const std::size_t i = next++;
			if (i >= todo.size() || run.Cancelled()) return;
			ProcessRequest(run, files, *todo[i], rng);
		}
	};
	const std::size_t n = std::min<std::size_t>(todo.size(), static_cast<std::size_t>(std::clamp(run.o.connections, 1, 16)));
	std::random_device rd;
	std::vector<std::thread> threads;
	for (std::size_t i = 1; i < n; ++i) threads.emplace_back(worker, rd());
	worker(rd());
	for (std::thread& t : threads) t.join();
}

} // namespace

bool RunPlan(const std::vector<ChannelSource>& sources, const Plan& plan, InstallIndex& index,
             const InstallOptions& options, InstallResult& out)
{
	out = InstallResult();
	if (!options.transport || options.root.empty() || options.downloadDir.empty())
	{
		out.failures.push_back("faltan el transporte, la carpeta del cliente o la de descargas");
		return false;
	}
	Run run{ sources, options, index, out };
	for (const RangeRequest& r : plan.requests)
		if (!options.selfOnly || r.self) run.status.downloadTotal += r.length;
	for (const PlanFile& f : plan.files)
		if (!options.selfOnly || f.entry->self) ++run.status.filesTotal;

	RunRequests(run, plan.files, plan.requests);

	// Deltas que no sirvieron: el archivo entero, un pedido por archivo.
	if (!run.fallback.empty() && !run.Cancelled())
	{
		const std::vector<PlanFile> files = run.fallback;
		std::vector<RangeRequest> requests;
		for (std::size_t i = 0; i < files.size(); ++i)
		{
			RangeRequest r;
			r.channel = files[i].channel;
			r.bundle = files[i].entry->bundle;
			r.offset = files[i].entry->offset;
			r.length = files[i].entry->packed;
			r.self = files[i].entry->self;
			r.required = files[i].entry->required;
			r.pieces.push_back(Piece{ i, 0, r.length });
			run.status.downloadTotal += r.length;
			requests.push_back(std::move(r));
		}
		RunRequests(run, files, requests);
	}

	if (!options.selfOnly && !run.Cancelled())
		for (const std::string& rel : plan.remove)
		{
			if (!RemoveFile(JoinPath(options.root, rel)))
			{
				out.failures.push_back(rel + ": no se pudo borrar");
				continue;
			}
			index.files.erase(rel);
			++out.removed;
			run.dirty = true;
		}

	out.cancelled = run.Cancelled();
	{
		std::lock_guard<std::mutex> lock(run.mutex);
		run.SaveLocked();
		run.Report(true);
	}
	return out.failures.empty() && !out.cancelled;
}

} // namespace Update
