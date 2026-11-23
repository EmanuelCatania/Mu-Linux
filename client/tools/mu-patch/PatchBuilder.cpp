#include "PatchBuilder.h"

#include "update/Delta.h"
#include "update/FileUtil.h"
#include "update/Hash.h"
#include "update/Manifest.h"
#include "update/Zip.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>

namespace MuPatch {

using namespace Update;

namespace {

constexpr std::uint64_t kDeltaMinSize = 1ull << 20;    // archivos más chicos no llevan delta
constexpr std::uint64_t kDeltaMaxPercent = 70;          // un delta que no ahorra 30 % no se publica
constexpr std::size_t kDeltaHistory = 3;                // contra las 3 versiones anteriores
constexpr std::uint64_t kBatchBytes = 256ull << 20;     // memoria máxima por tanda de compresión
const char kMarker[] = "MuRelease1:";

void Say(const Log& log, const std::string& s)
{
	if (log) log(s);
}

bool Fail(std::string* error, const std::string& what)
{
	if (error) *error = what;
	return false;
}

std::string Slug(const std::string& channel)
{
	std::string s = channel;
	std::replace(s.begin(), s.end(), '/', '-');
	return s;
}

bool IsOverlay(const std::string& channel)
{
	const std::string suffix = "/overlay";
	return channel.size() > suffix.size() && channel.compare(channel.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool StartsWithNoCase(const std::string& s, const char* prefix)
{
	const std::size_t n = std::strlen(prefix);
	if (s.size() < n) return false;
	for (std::size_t i = 0; i < n; ++i)
		if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
	return true;
}

bool EndsWithNoCase(const std::string& s, const char* suffix)
{
	const std::size_t n = std::strlen(suffix);
	return s.size() >= n && StartsWithNoCase(s.substr(s.size() - n), suffix);
}

std::string Human(std::uint64_t bytes)
{
	char buf[32];
	if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof(buf), "%.2f MB", bytes / 1048576.0);
	else if (bytes >= 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
	else std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
	return buf;
}

bool LoadManifestDir(const std::string& dir, const std::uint8_t publicKey[kKeySize], Manifest& m, std::string* error)
{
	std::string text, sigText;
	if (!ReadWholeFile(JoinPath(dir, "manifest.json"), text)) return Fail(error, "no se puede leer " + JoinPath(dir, "manifest.json"));
	if (!ReadWholeFile(JoinPath(dir, "manifest.json.sig"), sigText)) return Fail(error, "no se puede leer " + JoinPath(dir, "manifest.json.sig"));
	std::uint8_t sig[kSignatureSize];
	if (!ParseSignatureFile(sigText, sig)) return Fail(error, "firma con formato no válido en " + dir);
	if (!Verify(kPatchContext, text.data(), text.size(), sig, publicKey))
		return Fail(error, "la firma de " + JoinPath(dir, "manifest.json") + " no corresponde a la clave");
	std::string parseError;
	if (!ParseManifest(text, m, &parseError)) return Fail(error, JoinPath(dir, "manifest.json") + ": " + parseError);
	return true;
}

const std::string& ZipDir(const PrevSource& p) { return p.zipDir.empty() ? p.dir : p.zipDir; }

// Lee de una versión anterior el contenido de un archivo (para armar deltas).
bool ReadFromManifest(const std::string& dir, const Manifest& m, const FileEntry& f, std::vector<std::uint8_t>& out)
{
	const std::string zip = JoinPath(dir, m.bundles[f.bundle].name);
	std::vector<std::uint8_t> packed;
	if (!ReadFileRange(zip, f.offset, f.packed, packed)) return false;
	std::string error;
	if (!ZipDecode(f.method, packed.data(), packed.size(), f.size, out, &error)) return false;
	return HashBytes(out.data(), out.size()) == f.hash;
}

struct Packed
{
	std::vector<std::uint8_t> data;
	ZipMethod method = ZipMethod::Store;
	std::uint32_t crc = 0;
	std::uint64_t size = 0;
	std::string error;
};

void PackBytes(const std::vector<std::uint8_t>& raw, int level, Packed& out)
{
	out.size = raw.size();
	out.crc = Crc32(raw.data(), raw.size());
	if (!raw.empty() && ZstdCompress(raw.data(), raw.size(), level, out.data, &out.error) && out.data.size() < raw.size())
	{
		out.method = ZipMethod::Zstd;
		return;
	}
	// Ya comprimido (jpg, ogg...) o vacío: se guarda tal cual.
	out.error.clear();
	out.method = ZipMethod::Store;
	out.data = raw;
}

// Escribe los zips de una serie, cortando al llegar al tamaño máximo.
class BundleSink
{
public:
	BundleSink(std::string outDir, std::string prefix, std::uint64_t limit, Manifest& manifest)
		: m_Out(std::move(outDir)), m_Prefix(std::move(prefix)), m_Limit(limit), m_Manifest(manifest) {}

	bool Add(const std::string& name, const Packed& p, std::uint32_t& bundle, std::uint64_t& offset, std::string* error)
	{
		if (m_Open && m_Count > 0 && m_Writer.Position() + p.data.size() + name.size() + 200 > m_Limit)
			if (!Close(error)) return false;
		if (!m_Open)
		{
			BundleRef ref;
			ref.name = m_Prefix + "-" + std::to_string(++m_Number) + ".zip";
			if (!m_Writer.Open(JoinPath(m_Out, ref.name))) return Fail(error, "no se puede crear " + ref.name);
			m_Index = static_cast<std::uint32_t>(m_Manifest.bundles.size());
			m_Manifest.bundles.push_back(ref);
			m_Open = true;
			m_Count = 0;
		}
		ZipEntry e;
		if (!m_Writer.Add(name, p.method, p.data.data(), p.data.size(), p.size, p.crc, &e))
			return Fail(error, "no se puede escribir " + m_Manifest.bundles[m_Index].name);
		++m_Count;
		bundle = m_Index;
		offset = e.dataOffset;
		return true;
	}

	bool Close(std::string* error)
	{
		if (!m_Open) return true;
		m_Open = false;
		BundleRef& ref = m_Manifest.bundles[m_Index];
		const std::string path = JoinPath(m_Out, ref.name);
		if (!m_Writer.Finish()) return Fail(error, "no se pudo terminar " + ref.name);
		if (!HashFile(path, ref.hash, &ref.size)) return Fail(error, "no se puede leer " + ref.name);
		return true;
	}

private:
	std::string m_Out, m_Prefix;
	std::uint64_t m_Limit;
	Manifest& m_Manifest;
	ZipWriter m_Writer;
	bool m_Open = false;
	std::uint32_t m_Index = 0;
	std::uint32_t m_Number = 0;
	std::size_t m_Count = 0;
};

struct InputFile
{
	std::string path;
	std::uint64_t size = 0;
	Digest hash{};
	std::string platform;
	bool required = true;
	bool self = false;
	// Si el mismo contenido ya está publicado en una versión anterior.
	int prev = -1;
	const FileEntry* prevEntry = nullptr;
};

bool MatchesAny(const std::vector<std::string>& globs, const std::string& path)
{
	for (const auto& g : globs)
		if (GlobMatch(g, path)) return true;
	return false;
}

} // namespace

namespace {

// p y s apuntan a strings terminados en '\0'.  Recursivo: los patrones son
// cortos y tienen pocos comodines.
bool GlobAt(const char* p, const char* s)
{
	for (;;)
	{
		if (*p == 0) return *s == 0;
		if (p[0] == '*' && p[1] == '*')
		{
			const char* rest = p + 2;
			// "**/" también vale para cero carpetas.
			if (*rest == '/' && GlobAt(rest + 1, s)) return true;
			for (const char* t = s;; ++t)
			{
				if (GlobAt(rest, t)) return true;
				if (*t == 0) return false;
			}
		}
		if (*p == '*')
		{
			for (const char* t = s;; ++t)
			{
				if (GlobAt(p + 1, t)) return true;
				if (*t == 0 || *t == '/') return false;
			}
		}
		if (*s == 0) return false;
		if (*p == '?') { if (*s == '/') return false; }
		else if (*p != *s) return false;
		++p;
		++s;
	}
}

} // namespace

bool GlobMatch(const std::string& pattern, const std::string& path)
{
	return GlobAt(pattern.c_str(), path.c_str());
}

std::string DetectPlatform(const std::vector<std::uint8_t>& d)
{
	if (d.size() >= 0x40 && d[0] == 'M' && d[1] == 'Z')
	{
		const std::uint32_t pe = d[0x3C] | (d[0x3D] << 8) | (d[0x3E] << 16) | (static_cast<std::uint32_t>(d[0x3F]) << 24);
		if (pe + 6 <= d.size() && std::memcmp(&d[pe], "PE\0\0", 4) == 0)
		{
			const unsigned machine = d[pe + 4] | (d[pe + 5] << 8);
			if (machine == 0x8664) return "windows-x64";
			if (machine == 0x014C) return "windows-x86";
			if (machine == 0xAA64) return "windows-arm64";
			return "windows";
		}
	}
	if (d.size() >= 20 && d[0] == 0x7F && d[1] == 'E' && d[2] == 'L' && d[3] == 'F')
	{
		const unsigned machine = d[5] == 2 ? (d[18] << 8 | d[19]) : (d[18] | d[19] << 8);
		if (machine == 62) return "linux-x64";
		if (machine == 3) return "linux-x86";
		if (machine == 183) return "linux-arm64";
		return "linux";
	}
	return std::string();
}

bool ReadReleaseMarker(const std::vector<std::uint8_t>& data, std::string& version)
{
	const std::size_t n = sizeof(kMarker) - 1;
	auto it = data.begin();
	while ((it = std::search(it, data.end(), kMarker, kMarker + n)) != data.end())
	{
		auto v = it + n;
		std::string s;
		while (v != data.end() && *v != 0 && s.size() < 33) s += static_cast<char>(*v++);
		if (v != data.end() && *v == 0 && IsValidVersion(s))
		{
			version = s;
			return true;
		}
		++it;
	}
	return false;
}

bool BuildPatch(const BuildOptions& o, std::string* error)
{
	if (!IsValidChannel(o.channel)) return Fail(error, "canal no válido (formato dueño/nombre, en minúsculas): " + o.channel);
	if (!IsValidVersion(o.version)) return Fail(error, "versión no válida: " + o.version);
	if (!o.base.empty() && !IsValidVersion(o.base)) return Fail(error, "--base no válida: " + o.base);
	if (o.level < 1 || o.level > 22) return Fail(error, "--level va de 1 a 22");
	if (o.bundleSize < (1ull << 20)) return Fail(error, "--bundle-size tiene que ser de al menos 1 MB");
	const bool overlay = IsOverlay(o.channel);

	std::vector<std::string> existing;
	std::string listError;
	if (FileExists(JoinPath(o.out, "manifest.json")) || (ListFiles(o.out, existing, &listError) && !existing.empty()))
		return Fail(error, "la carpeta de salida no está vacía: " + o.out);
	if (!MakeDirs(o.out)) return Fail(error, "no se puede crear " + o.out);

	KeyPair key;
	if (!LoadKeyFile(o.keyPath, key, error)) return false;

	// Versiones anteriores, de la más nueva a la más vieja.
	std::vector<std::pair<PrevSource, Manifest>> prevs;
	for (const PrevSource& p : o.prev)
	{
		Manifest m;
		if (!LoadManifestDir(p.dir, key.publicKey, m, error)) { key.Wipe(); return false; }
		if (m.channel != o.channel) { key.Wipe(); return Fail(error, p.dir + " es del canal " + m.channel + ", no de " + o.channel); }
		prevs.emplace_back(p, std::move(m));
	}
	std::sort(prevs.begin(), prevs.end(), [](const auto& a, const auto& b) { return a.second.sequence > b.second.sequence; });

	Manifest m;
	m.channel = o.channel;
	m.version = o.version;
	m.requiresProtocol = o.protocol;
	const std::uint64_t lastSequence = prevs.empty() ? 0 : prevs.front().second.sequence;
	m.sequence = o.sequence ? o.sequence : lastSequence + 1;
	if (m.sequence <= lastSequence)
	{
		key.Wipe();
		return Fail(error, "--sequence tiene que ser mayor que " + std::to_string(lastSequence) + " (la de la versión anterior)");
	}
	for (const auto& p : prevs)
	{
		if (p.second.version == o.version)
		{
			key.Wipe();
			return Fail(error, "la versión " + o.version + " ya está publicada en " + p.first.dir + ": los nombres de los zips chocarían");
		}
	}

	// ── Archivos de entrada ──────────────────────────────────────────────
	std::vector<std::string> paths;
	if (!ListFiles(o.in, paths, error)) { key.Wipe(); return false; }
	std::vector<InputFile> files;
	std::set<std::string> releases;
	bool anyExecutable = false;
	for (const std::string& path : paths)
	{
		if (!o.include.empty() && !MatchesAny(o.include, path)) continue;
		if (MatchesAny(o.exclude, path)) continue;
		if (!IsSafeRelativePath(path) || StartsWithNoCase(path, "User/"))
		{
			key.Wipe();
			return Fail(error, "ruta no permitida en un parche: " + path);
		}
		const std::string full = JoinPath(o.in, path);
		InputFile f;
		f.path = path;
		if (!HashFile(full, f.hash, &f.size)) { key.Wipe(); return Fail(error, "no se puede leer " + full); }
		std::vector<std::uint8_t> head;
		ReadFileRange(full, 0, std::min<std::uint64_t>(f.size, 4096), head);
		f.platform = DetectPlatform(head);
		if (!f.platform.empty())
		{
			anyExecutable = true;
			if (!overlay)
			{
				key.Wipe();
				return Fail(error, "el canal " + o.channel + " no puede traer ejecutables (" + path +
				                   "): cada operador publica el suyo en su overlay");
			}
			std::vector<std::uint8_t> data;
			std::string release;
			if (ReadWholeFile(full, data) && ReadReleaseMarker(data, release))
			{
				releases.insert(release);
				Say(o.log, path + ": cliente " + release + " (" + f.platform + ")");
			}
			f.self = StartsWithNoCase(path, "Launcher/");
		}
		if (overlay && EndsWithNoCase(path, ".toml"))
		{
			key.Wipe();
			return Fail(error, path + ": los .toml no van en el parche; el server los manda en el handshake "
			                   "(ponelos en Data/ClientContent)");
		}
		f.required = !MatchesAny(o.optional, path);
		files.push_back(std::move(f));
	}
	if (files.empty()) { key.Wipe(); return Fail(error, "no hay archivos para publicar en " + o.in); }

	// ── Versión base ─────────────────────────────────────────────────────
	if (overlay)
	{
		if (!anyExecutable)
		{
			key.Wipe();
			return Fail(error, "el overlay tiene que traer el ejecutable del cliente (lleva la dirección y la clave de tu server)");
		}
		if (releases.size() > 1)
		{
			std::string list;
			for (const auto& r : releases) list += (list.empty() ? "" : ", ") + r;
			key.Wipe();
			return Fail(error, "los ejecutables son de versiones distintas (" + list + "): compilalos desde el mismo tag");
		}
		if (o.base.empty() && releases.empty())
		{
			key.Wipe();
			return Fail(error, "el ejecutable no dice de qué versión es (es anterior a MU_RELEASE): indicala con --base");
		}
		m.requiresBase = o.base.empty() ? *releases.begin() : o.base;
		if (!o.base.empty() && !releases.empty() && *releases.begin() != o.base)
			Say(o.log, "AVISO: --base " + o.base + " pero el ejecutable es de " + *releases.begin() +
			           "; el launcher va a bajar la base " + o.base);
	}
	else
	{
		m.requiresBase = o.base;
	}

	// ── Lo que ya está publicado se reusa ───────────────────────────────
	for (InputFile& f : files)
	{
		for (std::size_t i = 0; i < prevs.size() && f.prev < 0; ++i)
		{
			const Manifest& pm = prevs[i].second;
			for (const FileEntry& e : pm.files)
			{
				if (e.hash != f.hash || e.size != f.size) continue;
				const BundleRef& b = pm.bundles[e.bundle];
				const bool reachable = !b.url.empty() || !prevs[i].first.url.empty() ||
				                       FileExists(JoinPath(ZipDir(prevs[i].first), b.name));
				if (!reachable) continue;
				f.prev = static_cast<int>(i);
				f.prevEntry = &e;
				break;
			}
		}
	}

	BundleSink sink(o.out, Slug(o.channel) + "-" + o.version, o.bundleSize, m);
	BundleSink deltaSink(o.out, Slug(o.channel) + "-" + o.version + "-delta", o.bundleSize, m);
	std::map<std::pair<int, std::uint32_t>, std::uint32_t> reusedBundles;
	m.files.resize(files.size());
	std::uint64_t newBytes = 0, reusedCount = 0;

	for (std::size_t i = 0; i < files.size(); ++i)
	{
		const InputFile& f = files[i];
		FileEntry& e = m.files[i];
		e.path = f.path;
		e.size = f.size;
		e.hash = f.hash;
		e.required = f.required;
		e.platform = f.platform;
		e.self = f.self;
		if (f.prev < 0) continue;

		const auto& src = prevs[static_cast<std::size_t>(f.prev)];
		const BundleRef& old = src.second.bundles[f.prevEntry->bundle];
		const auto key2 = std::make_pair(f.prev, f.prevEntry->bundle);
		auto found = reusedBundles.find(key2);
		if (found == reusedBundles.end())
		{
			BundleRef ref = old;
			if (ref.url.empty() && !src.first.url.empty()) ref.url = JoinPath(src.first.url, old.name);
			if (ref.url.empty() && !o.reuseInPlace && !CopyFileTo(JoinPath(ZipDir(src.first), old.name), JoinPath(o.out, old.name)))
			{
				key.Wipe();
				return Fail(error, "no se puede copiar " + old.name);
			}
			for (const BundleRef& b : m.bundles)
				if (b.name == ref.name) { key.Wipe(); return Fail(error, "zip repetido entre versiones: " + ref.name); }
			found = reusedBundles.emplace(key2, static_cast<std::uint32_t>(m.bundles.size())).first;
			m.bundles.push_back(ref);
		}
		e.bundle = found->second;
		e.offset = f.prevEntry->offset;
		e.packed = f.prevEntry->packed;
		e.method = f.prevEntry->method;
		++reusedCount;
	}

	// ── Archivos nuevos: se comprimen en paralelo, por tandas ─────────────
	unsigned threads = o.threads > 0 ? static_cast<unsigned>(o.threads) : std::thread::hardware_concurrency();
	if (threads == 0) threads = 2;
	std::vector<std::size_t> pending;
	for (std::size_t i = 0; i < files.size(); ++i)
		if (files[i].prev < 0) pending.push_back(i);

	std::size_t next = 0;
	while (next < pending.size())
	{
		std::vector<std::size_t> batch;
		std::uint64_t batchBytes = 0;
		while (next < pending.size() && (batch.empty() || batchBytes + files[pending[next]].size <= kBatchBytes))
		{
			batchBytes += files[pending[next]].size;
			batch.push_back(pending[next++]);
		}
		std::vector<Packed> packed(batch.size());
		std::atomic<std::size_t> cursor{ 0 };
		auto worker = [&]() {
			for (std::size_t k; (k = cursor.fetch_add(1)) < batch.size();)
			{
				const InputFile& f = files[batch[k]];
				std::vector<std::uint8_t> raw;
				if (!ReadWholeFile(JoinPath(o.in, f.path), raw)) { packed[k].error = "no se puede leer " + f.path; continue; }
				if (raw.size() != f.size || HashBytes(raw.data(), raw.size()) != f.hash)
				{
					packed[k].error = f.path + " cambió mientras se armaba el parche";
					continue;
				}
				PackBytes(raw, o.level, packed[k]);
			}
		};
		std::vector<std::thread> pool;
		for (unsigned t = 1; t < std::min<std::size_t>(threads, batch.size()); ++t) pool.emplace_back(worker);
		worker();
		for (auto& t : pool) t.join();

		for (std::size_t k = 0; k < batch.size(); ++k)
		{
			if (!packed[k].error.empty()) { key.Wipe(); return Fail(error, packed[k].error); }
			FileEntry& e = m.files[batch[k]];
			e.method = packed[k].method;
			e.packed = packed[k].data.size();
			if (!sink.Add(e.path, packed[k], e.bundle, e.offset, error)) { key.Wipe(); return false; }
			newBytes += e.packed;
		}
	}
	if (!sink.Close(error)) { key.Wipe(); return false; }

	// ── Deltas contra las versiones anteriores ────────────────────────────
	std::uint64_t deltaCount = 0, deltaBytes = 0;
	for (std::size_t idx : pending)
	{
		FileEntry& e = m.files[idx];
		if (e.size < kDeltaMinSize || e.size > kMaxDeltaFileSize) continue;
		std::vector<std::uint8_t> raw;
		std::set<std::string> done;
		for (std::size_t i = 0; i < prevs.size() && i < kDeltaHistory; ++i)
		{
			const Manifest& pm = prevs[i].second;
			for (const FileEntry& old : pm.files)
			{
				if (old.platform != e.platform || old.path != e.path || old.hash == e.hash) continue;
				if (old.size > kMaxDeltaFileSize || !done.insert(FormatDigest(old.hash)).second) continue;
				std::vector<std::uint8_t> oldData;
				if (!FileExists(JoinPath(ZipDir(prevs[i].first), pm.bundles[old.bundle].name)) ||
				    !ReadFromManifest(ZipDir(prevs[i].first), pm, old, oldData))
				{
					Say(o.log, "sin delta para " + e.path + " desde " + pm.version + ": no está su zip en " + ZipDir(prevs[i].first));
					continue;
				}
				if (raw.empty() && !ReadWholeFile(JoinPath(o.in, e.path), raw)) { key.Wipe(); return Fail(error, "no se puede leer " + e.path); }
				Packed patch;
				std::string deltaError;
				if (!DeltaCreate(oldData, raw, o.level, patch.data, &deltaError))
				{
					Say(o.log, "sin delta para " + e.path + ": " + deltaError);
					continue;
				}
				if (patch.data.size() * 100 >= e.packed * kDeltaMaxPercent) continue;
				patch.method = ZipMethod::Store;
				patch.size = patch.data.size();
				patch.crc = Crc32(patch.data.data(), patch.data.size());
				DeltaRef d;
				d.from = old.hash;
				d.packed = patch.data.size();
				const std::string name = "deltas/" + (e.platform.empty() ? std::string() : e.platform + "/") + e.path + "@" + ToHex(old.hash.data(), 8) + ".zstpatch";
				if (!deltaSink.Add(name, patch, d.bundle, d.offset, error)) { key.Wipe(); return false; }
				e.deltas.push_back(d);
				++deltaCount;
				deltaBytes += d.packed;
				Say(o.log, "delta " + e.path + " desde " + pm.version + ": " + Human(d.packed) + " (entero " + Human(e.packed) + ")");
			}
		}
	}
	if (!deltaSink.Close(error)) { key.Wipe(); return false; }

	// ── Borrados y plataformas ────────────────────────────────────────────
	for (const std::string& d : o.deleted)
	{
		if (!IsSafeRelativePath(d) || StartsWithNoCase(d, "User/")) { key.Wipe(); return Fail(error, "ruta a borrar no permitida: " + d); }
		m.deleted.push_back(d);
	}
	std::set<std::string> platforms;
	for (const FileEntry& e : m.files)
		if (!e.platform.empty()) platforms.insert(e.platform);
	m.platforms.assign(platforms.begin(), platforms.end());

	if (overlay && !o.baseManifest.empty())
	{
		std::string text;
		Manifest bm;
		std::string parseError;
		if (ReadWholeFile(o.baseManifest, text) && ParseManifest(text, bm, &parseError))
		{
			for (const FileEntry& e : m.files)
				for (const FileEntry& b : bm.files)
					if (b.path == e.path) Say(o.log, "pisa a la base: " + e.path);
		}
		else
		{
			Say(o.log, "AVISO: no se pudo leer " + o.baseManifest + " " + parseError);
		}
	}

	// ── Manifiesto y firma ────────────────────────────────────────────────
	const std::string text = DumpManifest(m);
	Manifest check;
	std::string checkError;
	if (!ParseManifest(text, check, &checkError)) { key.Wipe(); return Fail(error, "el manifiesto generado no es válido: " + checkError); }
	std::uint8_t sig[kSignatureSize];
	Sign(kPatchContext, text.data(), text.size(), key, sig);
	key.Wipe();
	const std::string sigText = FormatSignatureFile(sig);
	if (!WriteFileAtomic(JoinPath(o.out, "manifest.json"), text.data(), text.size()) ||
	    !WriteFileAtomic(JoinPath(o.out, "manifest.json.sig"), sigText.data(), sigText.size()))
		return Fail(error, "no se puede escribir el manifiesto en " + o.out);

	Say(o.log, std::to_string(m.files.size()) + " archivos (" + std::to_string(reusedCount) + " de versiones anteriores), " +
	           std::to_string(m.bundles.size()) + " zips, " + Human(newBytes) + " nuevos, " +
	           std::to_string(deltaCount) + " deltas (" + Human(deltaBytes) + "), secuencia " + std::to_string(m.sequence));
	return true;
}

bool VerifyPatch(const VerifyOptions& o, std::string* error)
{
	Manifest m;
	if (!LoadManifestDir(o.in, o.publicKey, m, error)) return false;

	std::vector<std::pair<PrevSource, Manifest>> prevs;
	for (const PrevSource& p : o.prev)
	{
		Manifest pm;
		if (!LoadManifestDir(p.dir, o.publicKey, pm, error)) return false;
		prevs.emplace_back(p, std::move(pm));
	}

	std::vector<bool> local(m.bundles.size(), false);
	std::vector<ZipReader> readers(m.bundles.size());
	std::size_t remote = 0;
	for (std::size_t i = 0; i < m.bundles.size(); ++i)
	{
		const BundleRef& b = m.bundles[i];
		const std::string path = JoinPath(o.in, b.name);
		if (!FileExists(path))
		{
			if (b.url.empty()) return Fail(error, "falta " + b.name + " (no tiene url y no está junto al manifiesto)");
			++remote;
			continue;
		}
		Digest h;
		std::uint64_t size = 0;
		if (!HashFile(path, h, &size)) return Fail(error, "no se puede leer " + path);
		if (size != b.size || h != b.hash) return Fail(error, b.name + " no coincide con el manifiesto");
		if (!readers[i].Open(path, error)) return false;
		local[i] = true;
	}

	auto findEntry = [&](std::uint32_t bundle, std::uint64_t offset) -> const ZipEntry* {
		for (const ZipEntry& z : readers[bundle].Entries())
			if (z.dataOffset == offset) return &z;
		return nullptr;
	};

	std::size_t checked = 0, skipped = 0, deltasChecked = 0, deltasApplied = 0;
	for (const FileEntry& f : m.files)
	{
		if (!local[f.bundle]) { ++skipped; continue; }
		const ZipEntry* z = findEntry(f.bundle, f.offset);
		if (!z || z->packedSize != f.packed || z->size != f.size || z->method != f.method)
			return Fail(error, f.path + ": el tramo del manifiesto no es una entrada de " + m.bundles[f.bundle].name);
		std::vector<std::uint8_t> data;
		if (!ZipExtract(JoinPath(o.in, m.bundles[f.bundle].name), *z, data, error)) return false;
		if (HashBytes(data.data(), data.size()) != f.hash) return Fail(error, f.path + ": hash incorrecto");
		++checked;

		for (const DeltaRef& d : f.deltas)
		{
			if (!local[d.bundle]) continue;
			const ZipEntry* dz = findEntry(d.bundle, d.offset);
			if (!dz || dz->method != ZipMethod::Store || dz->packedSize != d.packed)
				return Fail(error, f.path + ": el delta no es una entrada de " + m.bundles[d.bundle].name);
			std::vector<std::uint8_t> patch;
			if (!ZipExtract(JoinPath(o.in, m.bundles[d.bundle].name), *dz, patch, error)) return false;
			++deltasChecked;
			for (const auto& p : prevs)
			{
				const FileEntry* old = nullptr;
				for (const FileEntry& e : p.second.files)
					if (e.hash == d.from) { old = &e; break; }
				std::vector<std::uint8_t> oldData;
				if (!old || !FileExists(JoinPath(ZipDir(p.first), p.second.bundles[old->bundle].name)) ||
				    !ReadFromManifest(ZipDir(p.first), p.second, *old, oldData)) continue;
				std::vector<std::uint8_t> rebuilt;
				if (!DeltaApply(oldData, patch.data(), patch.size(), f.size, rebuilt, error)) return false;
				if (HashBytes(rebuilt.data(), rebuilt.size()) != f.hash) return Fail(error, f.path + ": el delta no da el archivo nuevo");
				++deltasApplied;
				break;
			}
		}
	}

	Say(o.log, m.channel + " " + m.version + " (secuencia " + std::to_string(m.sequence) + "): firma correcta, " +
	           std::to_string(checked) + " archivos verificados" +
	           (skipped ? ", " + std::to_string(skipped) + " en zips remotos sin verificar (" + std::to_string(remote) + " zips)" : "") +
	           ", " + std::to_string(deltasChecked) + " deltas leídos, " + std::to_string(deltasApplied) + " aplicados");
	return true;
}

} // namespace MuPatch
