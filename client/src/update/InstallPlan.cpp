#include "update/InstallPlan.h"

#include "update/FileUtil.h"

#include <algorithm>
#include <map>
#include <numeric>
#include <set>

namespace Update {

namespace {

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

bool EndsWith(const std::string& s, const char* suffix)
{
	const std::size_t n = std::char_traits<char>::length(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// Los .toml de Content los instala la base la primera vez; después son de
// ContentSync (los baja el cliente en el saludo con el ConnectServer).  El
// launcher no los repara ni los borra: si no, los dos se pisarían.
bool InstallOnce(const std::string& path) { return EndsWith(path, ".toml"); }

struct Choice
{
	std::size_t source = 0;
	const FileEntry* entry = nullptr;
};

// Un tramo de un zip que hay que bajar.
struct Want
{
	std::size_t file = 0;
	std::uint64_t offset = 0;
	std::uint64_t packed = 0;
	int cls = 0;     // 0 launcher, 1 obligatorio, 2 opcional
};

} // namespace

std::string HdVariant(const std::string& path)
{
	const std::size_t slash = path.rfind('/');
	const std::size_t dot = path.rfind('.');
	if (dot == std::string::npos || (slash != std::string::npos && dot < slash) || dot == 0 ||
	    (slash != std::string::npos && dot == slash + 1))
		return std::string();
	const std::string stem = path.substr(0, dot);
	if (EndsWith(stem, "-hd")) return std::string();
	return stem + "-hd" + path.substr(dot);
}

std::vector<std::string> BundleUrls(const ChannelSource& source, std::uint32_t bundle)
{
	std::vector<std::string> urls;
	const BundleRef& b = source.manifest->bundles[bundle];
	if (!b.url.empty()) urls.push_back(b.url);
	for (const std::string& base : source.baseUrls)
	{
		std::string u = base;
		if (!u.empty() && u.back() != '/' && u.back() != '\\') u += '/';
		u += b.name;
		if (std::find(urls.begin(), urls.end(), u) == urls.end()) urls.push_back(u);
	}
	return urls;
}

bool BuildPlan(const std::vector<ChannelSource>& sources, const PlanOptions& options, InstallIndex& index,
               Plan& out, std::string* error)
{
	out = Plan();
	if (options.root.empty()) return Fail(error, "falta la carpeta del cliente");
	for (const ChannelSource& s : sources)
	{
		if (!s.manifest) return Fail(error, "fuente sin manifiesto");
		if (s.baseUrls.empty())
			for (const BundleRef& b : s.manifest->bundles)
				if (b.url.empty()) return Fail(error, s.manifest->channel + ": " + b.name + " no tiene url ni hay de dónde bajarlo");
	}

	// ── Capas ─────────────────────────────────────────────────────────────
	std::vector<std::size_t> order(sources.size());
	std::iota(order.begin(), order.end(), std::size_t(0));
	std::stable_sort(order.begin(), order.end(),
	                 [&](std::size_t a, std::size_t b) { return sources[a].priority > sources[b].priority; });

	std::map<std::string, Choice> chosen;
	std::set<std::string> hidden;   // "-hd" tapados por una capa de arriba
	for (std::size_t si : order)
	{
		const Manifest& m = *sources[si].manifest;
		std::vector<std::string> added;
		for (const FileEntry& f : m.files)
		{
			if (!f.platform.empty() && f.platform != options.platform) continue;
			if (hidden.count(f.path) || chosen.count(f.path)) continue;
			chosen[f.path] = Choice{ si, &f };
			added.push_back(f.path);
		}
		for (const std::string& p : added)
		{
			const std::string hd = HdVariant(p);
			if (!hd.empty() && !chosen.count(hd)) hidden.insert(hd);
		}
	}

	// ── Contra lo que hay en disco ────────────────────────────────────────
	std::vector<Want> wants;
	for (const auto& [path, c] : chosen)
	{
		const FileEntry& f = *c.entry;
		const std::string& channel = sources[c.source].manifest->channel;
		if (f.self && !options.includeSelf)
		{
			// Igual se anota: el launcher avisa que hay uno nuevo.
			Digest actual{};
			if (CheckLocal(options.root, path, f.hash, index, options.fullVerify, &actual) != LocalState::Match)
				out.selfUpdate = true;
			else
				++out.upToDate;
			continue;
		}
		const bool exists = FileExists(JoinPath(options.root, path));
		if (InstallOnce(path) && exists)
		{
			++out.upToDate;
			continue;
		}
		Digest actual{};
		const LocalState st = CheckLocal(options.root, path, f.hash, index, options.fullVerify, &actual);
		if (st == LocalState::Match)
		{
			index.files[path].channel = channel;
			++out.upToDate;
			continue;
		}
		if (options.gameRunning && st != LocalState::Missing)
		{
			++out.heldForGame;
			continue;
		}
		if (st == LocalState::Unreadable) out.unreadable.push_back(path);

		PlanFile pf;
		pf.channel = c.source;
		pf.entry = &f;
		if (st == LocalState::Differs)
			for (const DeltaRef& d : f.deltas)
				if (d.from == actual && d.packed < f.packed)
				{
					pf.delta = &d;
					break;
				}
		const std::size_t idx = out.files.size();
		out.files.push_back(pf);
		out.installBytes += f.size;
		Want w;
		w.file = idx;
		w.cls = f.self ? 0 : (f.required ? 1 : 2);
		if (pf.delta)
		{
			w.offset = pf.delta->offset;
			w.packed = pf.delta->packed;
		}
		else
		{
			w.offset = f.offset;
			w.packed = f.packed;
		}
		wants.push_back(w);
		if (f.self) out.selfUpdate = true;
	}

	// ── Pedidos ───────────────────────────────────────────────────────────
	std::map<std::pair<std::size_t, std::uint32_t>, std::vector<Want>> byBundle;
	for (const Want& w : wants)
	{
		const PlanFile& pf = out.files[w.file];
		const std::uint32_t bundle = pf.delta ? pf.delta->bundle : pf.entry->bundle;
		byBundle[{ pf.channel, bundle }].push_back(w);
	}
	for (auto& [key, list] : byBundle)
	{
		const ChannelSource& src = sources[key.first];
		const std::uint64_t bundleSize = src.manifest->bundles[key.second].size;
		std::uint64_t needed = 0;
		int bestCls = 2;
		for (const Want& w : list)
		{
			needed += w.packed;
			bestCls = std::min(bestCls, w.cls);
		}
		auto make = [&](std::uint64_t offset, int cls) {
			RangeRequest r;
			r.channel = key.first;
			r.bundle = key.second;
			r.offset = offset;
			r.self = cls == 0;
			r.required = cls <= 1;
			return r;
		};
		// Más de la mitad del zip: entero, en un solo pedido.
		if (needed * 2 > bundleSize)
		{
			RangeRequest r = make(0, bestCls);
			r.length = bundleSize;
			for (const Want& w : list) r.pieces.push_back(Piece{ w.file, w.offset, w.packed });
			out.requests.push_back(std::move(r));
			continue;
		}
		for (int cls = 0; cls <= 2; ++cls)
		{
			std::vector<Want> part;
			for (const Want& w : list)
				if (w.cls == cls) part.push_back(w);
			std::sort(part.begin(), part.end(), [](const Want& a, const Want& b) { return a.offset < b.offset; });
			RangeRequest cur;
			bool open = false;
			for (const Want& w : part)
			{
				const std::uint64_t end = cur.offset + cur.length;
				if (!open || w.offset > end + kRangeMergeGap)
				{
					if (open) out.requests.push_back(std::move(cur));
					cur = make(w.offset, cls);
					open = true;
				}
				cur.length = std::max(cur.offset + cur.length, w.offset + w.packed) - cur.offset;
				cur.pieces.push_back(Piece{ w.file, w.offset - cur.offset, w.packed });
			}
			if (open) out.requests.push_back(std::move(cur));
		}
	}
	auto cls = [](const RangeRequest& r) { return r.self ? 0 : (r.required ? 1 : 2); };
	std::stable_sort(out.requests.begin(), out.requests.end(), [&](const RangeRequest& a, const RangeRequest& b) {
		if (cls(a) != cls(b)) return cls(a) < cls(b);
		if (a.channel != b.channel) return a.channel < b.channel;
		if (a.bundle != b.bundle) return a.bundle < b.bundle;
		return a.offset < b.offset;
	});
	for (const RangeRequest& r : out.requests) out.downloadBytes += r.length;

	// ── Lo que hay que borrar ─────────────────────────────────────────────
	if (!options.gameRunning)
	{
		std::set<std::string> channels;
		for (const ChannelSource& s : sources) channels.insert(s.manifest->channel);
		std::set<std::string> remove;
		for (const auto& [path, e] : index.files)
			if (!chosen.count(path) && channels.count(e.channel) && !InstallOnce(path)) remove.insert(path);
		for (const ChannelSource& s : sources)
			for (const std::string& path : s.manifest->deleted)
				if (!chosen.count(path) && !InstallOnce(path) && FileExists(JoinPath(options.root, path))) remove.insert(path);
		out.remove.assign(remove.begin(), remove.end());
	}
	return true;
}

} // namespace Update
