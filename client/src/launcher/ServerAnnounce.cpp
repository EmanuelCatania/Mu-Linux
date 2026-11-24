#include "ServerAnnounce.h"

#include "Lang.h"

#include "net/ServerTrust.h"
#include "update/FileUtil.h"
#include "update/Signing.h"

#include <algorithm>
#include <cstring>

namespace Launcher {

using namespace Update;

namespace {

const char kHex[] = "0123456789abcdef";

std::string Hex(const std::uint8_t* p, std::size_t n)
{
	std::string s;
	for (std::size_t i = 0; i < n; ++i)
	{
		s += kHex[p[i] >> 4];
		s += kHex[p[i] & 15];
	}
	return s;
}

// Firma (64 bytes) + launcher.json, como lo manda el ConnectServer.
bool CheckBlob(const std::vector<std::uint8_t>& blob, const std::uint8_t key[kKeySize], Announce& out, std::string* error)
{
	if (blob.size() <= kSignatureSize)
	{
		if (error) *error = Tr("respuesta vacía");
		return false;
	}
	const std::string text(blob.begin() + kSignatureSize, blob.end());
	if (!Verify(kLauncherContext, text.data(), text.size(), blob.data(), key))
	{
		if (error) *error = Tr("la firma no es del server");
		return false;
	}
	return ParseAnnounce(text, out, error);
}

void AddUnique(std::vector<std::string>& list, const std::string& v)
{
	if (!v.empty() && std::find(list.begin(), list.end(), v) == list.end()) list.push_back(v);
}

std::string TrimSlash(std::string u)
{
	while (!u.empty() && u.back() == '/') u.pop_back();
	return u;
}

} // namespace

std::vector<std::string> SplitList(const std::string& s)
{
	std::vector<std::string> out;
	std::size_t start = 0;
	while (start <= s.size())
	{
		std::size_t end = s.find(',', start);
		if (end == std::string::npos) end = s.size();
		std::string item = s.substr(start, end - start);
		const std::size_t a = item.find_first_not_of(" \t"), b = item.find_last_not_of(" \t");
		if (a != std::string::npos) out.push_back(item.substr(a, b - a + 1));
		start = end + 1;
	}
	return out;
}

AnnounceResult ResolveAnnounce(const AnnounceSeed& seed, const std::string& cacheDir, CsFetcher& cs, Transport& http,
                               const std::atomic<bool>* cancel)
{
	AnnounceResult r;
	std::uint8_t root[kKeySize];
	if (seed.rootKey.empty() || !ParsePublicKey(seed.rootKey, root)) return r;

	const std::string chainPath = JoinPath(cacheDir, "server-" + Hex(root, 8) + ".chain");
	const std::string cachePath = JoinPath(cacheDir, "launcher.json");
	std::string chain;
	ReadWholeFile(chainPath, chain);
	ServerTrust::ChainTip(root, chain, r.identity);
	r.haveIdentity = true;

	// 1. El guardado.
	Announce cached;
	bool haveCached = false;
	{
		std::vector<std::uint8_t> blob;
		if (ReadWholeFile(cachePath, blob) && CheckBlob(blob, r.identity, cached, nullptr)) haveCached = true;
	}
	auto accept = [&](const Announce& a, const std::vector<std::uint8_t>& blob) {
		if (haveCached && a.sequence < cached.sequence) return false;   // una configuración vieja
		if (!haveCached || a.sequence > cached.sequence) WriteFileAtomic(cachePath, blob.data(), blob.size());
		r.have = r.fresh = true;
		r.data = a;
		return true;
	};

	// 2. Los ConnectServer.
	std::vector<std::string> servers;
	if (haveCached)
		for (const std::string& s : cached.connectServers) AddUnique(servers, s);
	for (const std::string& s : seed.connectServers) AddUnique(servers, s);
	const auto trust = [&](const std::uint8_t* id, const std::uint8_t* prev, const std::uint8_t* sig) {
		std::string c = chain;
		const ServerTrust::Result res = ServerTrust::DecideFromRoot(root, c, id, prev, sig);
		if (res == ServerTrust::Result::Rotated)
		{
			MakeDirs(cacheDir);
			WriteFileAtomic(chainPath, c.data(), c.size());
			chain = c;
			std::memcpy(r.identity, id, kKeySize);
		}
		return res == ServerTrust::Result::Trusted || res == ServerTrust::Result::Rotated;
	};
	MakeDirs(cacheDir);
	for (const std::string& s : servers)
	{
		if (cancel && cancel->load()) return r;
		std::string host, why;
		std::uint16_t port = 0;
		if (!ParseHostPort(s, &host, &port)) continue;
		std::vector<std::uint8_t> blob;
		std::uint8_t identity[kKeySize];
		Announce a;
		if (!cs.Fetch(host, port, trust, blob, identity, &why) || !CheckBlob(blob, identity, a, &why))
		{
			r.notes.push_back(s + ": " + why);
			continue;
		}
		if (accept(a, blob)) return r;
		r.notes.push_back(Fmt("{0}: configuración más vieja que la guardada", { s }));
	}

	// 3. /updates/ de cada URL de parches.
	std::vector<std::string> urls;
	if (haveCached)
		for (const std::string& u : cached.patchUrls) AddUnique(urls, TrimSlash(u));
	for (const std::string& u : seed.urls) AddUnique(urls, TrimSlash(u));
	for (const std::string& u : urls)
	{
		if (cancel && cancel->load()) return r;
		std::vector<std::uint8_t> json, sigText;
		std::string why;
		if (!http.Get(u + "/launcher.json", kAnnounceMaxSize, json, &why) ||
		    !http.Get(u + "/launcher.json.sig", 1024, sigText, &why))
		{
			r.notes.push_back(u + ": " + why);
			continue;
		}
		std::uint8_t sig[kSignatureSize];
		if (!ParseSignatureFile(std::string(sigText.begin(), sigText.end()), sig))
		{
			r.notes.push_back(Fmt("{0}: firma ilegible", { u }));
			continue;
		}
		std::vector<std::uint8_t> blob(sig, sig + kSignatureSize);
		blob.insert(blob.end(), json.begin(), json.end());
		Announce a;
		if (!CheckBlob(blob, r.identity, a, &why))
		{
			r.notes.push_back(u + ": " + why);
			continue;
		}
		if (accept(a, blob)) return r;
		r.notes.push_back(Fmt("{0}: configuración más vieja que la guardada", { u }));
	}

	if (haveCached)
	{
		r.have = true;
		r.data = cached;
	}
	return r;
}

} // namespace Launcher
