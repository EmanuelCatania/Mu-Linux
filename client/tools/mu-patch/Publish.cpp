#include "Publish.h"

#include "update/Announce.h"
#include "update/FileUtil.h"
#include "update/Hash.h"
#include "update/Manifest.h"
#include "update/Signing.h"
#include "update/Zip.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>

namespace fs = std::filesystem;

namespace MuPatch {

using namespace Update;

namespace {

constexpr std::size_t kHistory = 3;   // versiones que se guardan (para deltas y para quien bajaba la anterior)

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

void Say(const Log& log, const std::string& s)
{
	if (log) log(s);
}

fs::path P(const std::string& utf8)
{
#if defined(__cpp_char8_t)
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
	return fs::u8path(utf8);
#endif
}

std::string U(const fs::path& p)
{
	const auto s = p.generic_u8string();
	return std::string(s.begin(), s.end());
}

std::string Lower(std::string s)
{
	for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool LoadSigned(const std::string& dir, const std::uint8_t publicKey[kKeySize], Manifest& m)
{
	std::string text, sigText, why;
	std::uint8_t sig[kSignatureSize];
	return ReadWholeFile(JoinPath(dir, "manifest.json"), text) && ReadWholeFile(JoinPath(dir, "manifest.json.sig"), sigText) &&
	       ParseSignatureFile(sigText, sig) && Verify(kPatchContext, text.data(), text.size(), sig, publicKey) &&
	       ParseManifest(text, m, &why);
}

// Las carpetas de history/, de la secuencia más alta a la más baja.
std::vector<std::pair<std::uint64_t, std::string>> HistoryDirs(const std::string& history)
{
	std::vector<std::pair<std::uint64_t, std::string>> out;
	std::error_code ec;
	for (fs::directory_iterator it(P(history), ec), end; !ec && it != end; it.increment(ec))
	{
		if (!it->is_directory(ec)) continue;
		const std::string name = U(it->path().filename());
		if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos) continue;
		out.emplace_back(std::stoull(name), U(it->path()));
	}
	std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
	return out;
}

// Junta un parche en staging.  deleted queda con las rutas que se borran.
bool ApplyPatch(const std::string& dir, const std::string& staging, std::set<std::string>& deleted, std::string* error)
{
	std::vector<std::string> files;
	if (!ListFiles(dir, files, error)) return false;
	// deleted.txt primero: saca lo de parches anteriores; lo que trae esta misma
	// carpeta queda.
	for (const std::string& rel : files)
	{
		if (Lower(rel) != "deleted.txt") continue;
		std::string text;
		if (!ReadWholeFile(JoinPath(dir, rel), text)) return Fail(error, "no se puede leer " + JoinPath(dir, rel));
		std::size_t pos = 0;
		while (pos < text.size())
		{
			std::size_t nl = text.find('\n', pos);
			if (nl == std::string::npos) nl = text.size();
			std::string line = text.substr(pos, nl - pos);
			pos = nl + 1;
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
			std::size_t a = 0;
			while (a < line.size() && (line[a] == ' ' || line[a] == '\t')) ++a;
			line = line.substr(a);
			std::replace(line.begin(), line.end(), '\\', '/');
			if (line.empty() || line[0] == '#') continue;
			if (!IsSafeRelativePath(line)) return Fail(error, JoinPath(dir, rel) + ": ruta no válida: " + line);
			RemoveFile(JoinPath(staging, line));
			deleted.insert(line);
		}
	}
	for (const std::string& rel : files)
	{
		const std::string lower = Lower(rel);
		if (lower == "deleted.txt") continue;
		const std::string src = JoinPath(dir, rel);
		if (rel.find('/') == std::string::npos && lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".zip") == 0)
		{
			ZipReader zip;
			std::string why;
			if (!zip.Open(src, &why)) return Fail(error, src + ": " + why);
			for (const ZipEntry& e : zip.Entries())
			{
				if (!e.name.empty() && e.name.back() == '/') continue;   // carpeta
				if (!IsSafeRelativePath(e.name)) return Fail(error, src + ": ruta no válida adentro del zip: " + e.name);
				std::vector<std::uint8_t> data;
				if (!ZipExtract(src, e, data, &why)) return Fail(error, src + ": " + why);
				if (!WriteFileAtomic(JoinPath(staging, e.name), data.data(), data.size()))
					return Fail(error, "no se puede escribir " + JoinPath(staging, e.name));
				deleted.erase(e.name);
			}
			continue;
		}
		if (!CopyFileTo(src, JoinPath(staging, rel))) return Fail(error, "no se puede copiar " + src);
		deleted.erase(rel);
	}
	return true;
}

} // namespace

bool ParsePatchFolder(const std::string& name, std::uint64_t* order)
{
	if (name.size() < 10 || name[8] != '-') return false;
	for (std::size_t i = 0; i < name.size(); ++i)
		if (i != 8 && !std::isdigit(static_cast<unsigned char>(name[i]))) return false;
	if (name.size() > 14) return false;
	const int dd = std::stoi(name.substr(0, 2)), mm = std::stoi(name.substr(2, 2)), yyyy = std::stoi(name.substr(4, 4));
	const std::uint64_t n = std::stoull(name.substr(9));
	if (dd < 1 || dd > 31 || mm < 1 || mm > 12 || yyyy < 2000 || n < 1) return false;
	if (order) *order = ((static_cast<std::uint64_t>(yyyy) * 100 + static_cast<std::uint64_t>(mm)) * 100 + static_cast<std::uint64_t>(dd)) * 100000 + n;
	return true;
}

std::string SnapshotPatches(const std::string& patches, const std::string& out)
{
	std::string snap;
	std::error_code ec;
	const fs::path outPath = P(out).lexically_normal();
	for (fs::recursive_directory_iterator it(P(patches), ec), end; !ec && it != end; it.increment(ec))
	{
		if (it->is_directory(ec) && it->path().lexically_normal() == outPath)
		{
			it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file(ec)) continue;
		snap += U(it->path().lexically_relative(P(patches))) + "\t" + std::to_string(it->file_size(ec)) + "\t" +
		        std::to_string(static_cast<long long>(it->last_write_time(ec).time_since_epoch().count())) + "\n";
	}
	return snap;
}

bool PublishPatches(const PublishOptions& o, bool* published, std::string* error)
{
	if (published) *published = false;
	if (o.patches.empty() || o.channel.empty() || o.keyPath.empty()) return Fail(error, "faltan --patches, --channel o --key");
	const std::string out = o.out.empty() ? JoinPath(o.patches, "published") : o.out;
	const std::string history = JoinPath(out, "history");
	const std::string staging = JoinPath(out, ".staging");
	const std::string fresh = JoinPath(out, ".new");
	KeyPair key;
	if (!LoadKeyFile(o.keyPath, key, error)) return false;
	std::uint8_t publicKey[kKeySize];
	std::copy(key.publicKey, key.publicKey + kKeySize, publicKey);
	key.Wipe();

	// ── Parches en orden ──────────────────────────────────────────────────
	std::vector<std::pair<std::uint64_t, std::string>> folders;
	std::error_code ec;
	const fs::path outPath = P(out).lexically_normal();
	for (fs::directory_iterator it(P(o.patches), ec), end; !ec && it != end; it.increment(ec))
	{
		if (!it->is_directory(ec) || it->path().lexically_normal() == outPath) continue;
		const std::string name = U(it->path().filename());
		std::uint64_t order = 0;
		if (!ParsePatchFolder(name, &order))
		{
			Say(o.log, "AVISO: " + name + " no tiene el formato DDMMAAAA-N; se ignora");
			continue;
		}
		folders.emplace_back(order, U(it->path()));
	}
	if (ec) return Fail(error, "no se puede leer " + o.patches + ": " + ec.message());
	std::sort(folders.begin(), folders.end());
	if (folders.empty()) return Fail(error, "no hay parches en " + o.patches);

	fs::remove_all(P(staging), ec);
	if (!MakeDirs(staging)) return Fail(error, "no se puede crear " + staging);
	std::set<std::string> deleted;
	for (const auto& f : folders)
		if (!ApplyPatch(f.second, staging, deleted, error)) return false;

	// ── ¿Cambió algo? ─────────────────────────────────────────────────────
	Manifest current;
	const bool haveCurrent = LoadSigned(out, publicKey, current);
	if (haveCurrent)
	{
		std::vector<std::string> files;
		if (!ListFiles(staging, files, error)) return false;
		std::map<std::string, Digest> now;
		for (const std::string& rel : files)
		{
			Digest d{};
			std::uint64_t size = 0;
			if (!HashFile(JoinPath(staging, rel), d, &size)) return Fail(error, "no se puede leer " + rel);
			now[rel] = d;
		}
		std::map<std::string, Digest> before;
		for (const FileEntry& e : current.files) before[e.path] = e.hash;
		const std::set<std::string> beforeDeleted(current.deleted.begin(), current.deleted.end());
		if (now == before && beforeDeleted == deleted)
		{
			fs::remove_all(P(staging), ec);
			Say(o.log, "sin cambios: sigue publicada la " + current.version + " (secuencia " + std::to_string(current.sequence) + ")");
			return true;
		}
	}

	// ── Versión nueva ─────────────────────────────────────────────────────
	const std::uint64_t sequence = haveCurrent ? current.sequence + 1 : 1;
	BuildOptions b;
	b.channel = o.channel;
	b.version = "1.0." + std::to_string(sequence);
	b.base = o.base;
	b.protocol = o.protocol;
	b.sequence = sequence;
	b.in = staging;
	b.out = fresh;
	b.keyPath = o.keyPath;
	b.deleted.assign(deleted.begin(), deleted.end());
	b.reuseInPlace = true;
	b.level = o.level;
	b.threads = o.threads;
	b.log = o.log;
	const auto hist = HistoryDirs(history);
	for (std::size_t i = 0; i < hist.size() && i < kHistory; ++i)
		b.prev.push_back(PrevSource{ hist[i].second, "", out });
	fs::remove_all(P(fresh), ec);
	if (!BuildPatch(b, error))
	{
		fs::remove_all(P(fresh), ec);
		return false;
	}

	// Los zips primero y el manifiesto al final: quien lo baje encuentra todo.
	std::vector<std::string> made;
	if (!ListFiles(fresh, made, error)) return false;
	for (const std::string& rel : made)
	{
		if (rel == "manifest.json" || rel == "manifest.json.sig") continue;
		std::string why;
		if (!MoveFileOver(JoinPath(fresh, rel), JoinPath(out, rel), &why)) return Fail(error, why);
	}
	const std::string histDir = JoinPath(history, std::to_string(sequence));
	if (!CopyFileTo(JoinPath(fresh, "manifest.json"), JoinPath(histDir, "manifest.json")) ||
	    !CopyFileTo(JoinPath(fresh, "manifest.json.sig"), JoinPath(histDir, "manifest.json.sig")))
		return Fail(error, "no se puede escribir " + histDir);
	std::string why;
	if (!MoveFileOver(JoinPath(fresh, "manifest.json.sig"), JoinPath(out, "manifest.json.sig"), &why) ||
	    !MoveFileOver(JoinPath(fresh, "manifest.json"), JoinPath(out, "manifest.json"), &why))
		return Fail(error, why);
	fs::remove_all(P(fresh), ec);
	fs::remove_all(P(staging), ec);
	if (published) *published = true;
	Say(o.log, "publicada la " + b.version + " (secuencia " + std::to_string(sequence) + ")");

	// ── Limpieza: zips que no usa ninguna de las últimas versiones ────────
	std::set<std::string> keep;
	const auto hist2 = HistoryDirs(history);
	for (std::size_t i = 0; i < hist2.size(); ++i)
	{
		if (i >= kHistory)
		{
			fs::remove_all(P(hist2[i].second), ec);
			continue;
		}
		Manifest m;
		if (!LoadSigned(hist2[i].second, publicKey, m)) continue;
		for (const BundleRef& r : m.bundles)
			if (r.url.empty()) keep.insert(r.name);
	}
	std::vector<std::string> present;
	if (ListFiles(out, present, nullptr))
		for (const std::string& rel : present)
		{
			if (rel.find('/') != std::string::npos || Lower(rel).size() < 4 || Lower(rel).compare(rel.size() - 4, 4, ".zip") != 0) continue;
			if (keep.count(rel)) continue;
			RemoveFile(JoinPath(out, rel));
			Say(o.log, "borrado " + rel + " (ya no lo usa ninguna versión)");
		}
	return true;
}

bool PublishAnnounce(const AnnounceOptions& o, bool* published, std::string* error)
{
	if (published) *published = false;
	if (o.out.empty() || o.keyPath.empty()) return Fail(error, "faltan la carpeta o la clave del launcher.json");
	KeyPair key;
	if (!LoadKeyFile(o.keyPath, key, error)) return false;

	const std::string path = JoinPath(o.out, "launcher.json");
	const std::string sigPath = path + ".sig";
	Announce current;
	bool haveCurrent = false, signedOk = false;
	std::string text, sigText;
	if (ReadWholeFile(path, text) && ParseAnnounce(text, current, nullptr))
	{
		haveCurrent = true;
		std::uint8_t sig[kSignatureSize];
		signedOk = ReadWholeFile(sigPath, sigText) && ParseSignatureFile(sigText, sig) &&
		           Verify(kLauncherContext, text.data(), text.size(), sig, key.publicKey);
	}
	if (signedOk && SameAnnounceContent(current, o.content))
	{
		key.Wipe();
		return true;
	}

	Announce next = o.content;
	next.sequence = haveCurrent ? current.sequence + 1 : 1;
	const std::string json = DumpAnnounce(next);
	Announce check;
	std::string why;
	if (!ParseAnnounce(json, check, &why))
	{
		key.Wipe();
		return Fail(error, "launcher.json no válido: " + why);
	}
	std::uint8_t sig[kSignatureSize];
	Sign(kLauncherContext, json.data(), json.size(), key, sig);
	key.Wipe();
	const std::string sigFile = FormatSignatureFile(sig);
	if (!MakeDirs(o.out) || !WriteFileAtomic(path, json.data(), json.size()) ||
	    !WriteFileAtomic(sigPath, sigFile.data(), sigFile.size()))
		return Fail(error, "no se puede escribir " + path);
	if (published) *published = true;
	Say(o.log, "launcher.json publicado (secuencia " + std::to_string(next.sequence) + ")");
	return true;
}

} // namespace MuPatch
