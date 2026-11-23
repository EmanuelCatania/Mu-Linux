#include "Announce.h"

#include "Json.h"
#include "Manifest.h"
#include "Signing.h"

namespace Update {

namespace {

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

bool IsHostChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool IsValidUrl(const std::string& u)
{
	if (u.size() > 512) return false;
	if (u.compare(0, 7, "http://") != 0 && u.compare(0, 8, "https://") != 0) return false;
	for (unsigned char c : u)
		if (c <= 0x20 || c == 0x7F || c == '"' || c == '\\') return false;
	return u.find("://") + 3 < u.size();
}

// Texto libre: UTF-8 sin caracteres de control salvo el salto de línea.
bool IsValidNews(const std::string& s)
{
	if (s.size() > kAnnounceMaxNews) return false;
	for (unsigned char c : s)
		if ((c < 0x20 && c != '\n') || c == 0x7F) return false;
	return true;
}

bool IsValidKeyHex(const std::string& k)
{
	std::uint8_t key[kKeySize];
	return ParsePublicKey(k, key);
}

bool StringList(const JsonValue& root, const char* name, std::size_t max, std::vector<std::string>& out,
                bool (*valid)(const std::string&), std::string* error)
{
	const JsonValue* v = root.Find(name);
	if (!v) return true;
	if (!v->IsArray()) return Fail(error, std::string("\"") + name + "\" tiene que ser una lista");
	if (v->Items().size() > max) return Fail(error, std::string("\"") + name + "\" tiene demasiadas entradas");
	for (const JsonValue& i : v->Items())
	{
		if (!i.IsString() || !valid(i.AsString())) return Fail(error, std::string("valor no válido en \"") + name + "\"");
		out.push_back(i.AsString());
	}
	return true;
}

bool OptString(const JsonValue& obj, const char* name, std::string& out, bool (*valid)(const std::string&),
               std::string* error)
{
	const JsonValue* v = obj.Find(name);
	if (!v) return true;
	if (!v->IsString() || !valid(v->AsString())) return Fail(error, std::string("\"") + name + "\" no es válido");
	out = v->AsString();
	return true;
}

bool IsHostPort(const std::string& s)
{
	return ParseHostPort(s, nullptr, nullptr);
}

bool IsBaseSource(const std::string& s)
{
	if (s.find("://") != std::string::npos) return IsValidUrl(s);
	if (s.empty() || s.size() > 100) return false;
	std::size_t slashes = 0;
	for (char c : s)
	{
		if (c == '/') ++slashes;
		else if (!IsHostChar(c) && c != '_') return false;
	}
	return slashes == 1 && s.front() != '/' && s.back() != '/';
}

} // namespace

bool ParseHostPort(const std::string& text, std::string* host, std::uint16_t* port)
{
	const std::size_t colon = text.rfind(':');
	if (colon == std::string::npos || colon == 0 || colon > 253) return false;
	const std::string h = text.substr(0, colon), p = text.substr(colon + 1);
	for (char c : h)
		if (!IsHostChar(c)) return false;
	if (h.front() == '.' || h.front() == '-' || h.back() == '.' || p.empty() || p.size() > 5) return false;
	long n = 0;
	for (char c : p)
	{
		if (c < '0' || c > '9') return false;
		n = n * 10 + (c - '0');
	}
	if (n < 1 || n > 65535) return false;
	if (host) *host = h;
	if (port) *port = static_cast<std::uint16_t>(n);
	return true;
}

std::string DumpAnnounce(const Announce& a)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("format", JsonValue::MakeInt(kAnnounceFormat));
	root.Set("sequence", JsonValue::MakeInt(static_cast<std::int64_t>(a.sequence)));
	if (!a.connectServers.empty())
	{
		JsonValue list = JsonValue::MakeArray();
		for (const std::string& s : a.connectServers) list.Push(JsonValue::MakeString(s));
		root.Set("connectServers", list);
	}
	if (!a.patchChannel.empty() || !a.patchUrls.empty())
	{
		JsonValue p = JsonValue::MakeObject();
		if (!a.patchChannel.empty()) p.Set("channel", JsonValue::MakeString(a.patchChannel));
		JsonValue urls = JsonValue::MakeArray();
		for (const std::string& u : a.patchUrls) urls.Push(JsonValue::MakeString(u));
		p.Set("urls", urls);
		root.Set("patches", p);
	}
	if (!a.baseSource.empty() || !a.baseTag.empty() || !a.baseKey.empty())
	{
		JsonValue b = JsonValue::MakeObject();
		if (!a.baseSource.empty()) b.Set("source", JsonValue::MakeString(a.baseSource));
		if (!a.baseTag.empty()) b.Set("tag", JsonValue::MakeString(a.baseTag));
		if (!a.baseKey.empty()) b.Set("key", JsonValue::MakeString(a.baseKey));
		root.Set("base", b);
	}
	if (!a.news.empty()) root.Set("news", JsonValue::MakeString(a.news));
	if (!a.web.empty()) root.Set("web", JsonValue::MakeString(a.web));
	return root.Dump();
}

bool ParseAnnounce(const std::string& text, Announce& out, std::string* error)
{
	if (text.size() > kAnnounceMaxSize) return Fail(error, "launcher.json es demasiado grande");
	JsonValue root;
	std::string jsonError;
	if (!ParseJson(text, root, &jsonError)) return Fail(error, "launcher.json no es JSON válido: " + jsonError);
	if (!root.IsObject()) return Fail(error, "launcher.json tiene que ser un objeto");
	const JsonValue* format = root.Find("format");
	if (!format || !format->IsInt()) return Fail(error, "falta \"format\"");
	if (format->AsInt() != kAnnounceFormat) return Fail(error, "formato de launcher.json desconocido: " + std::to_string(format->AsInt()));
	const JsonValue* seq = root.Find("sequence");
	if (!seq || !seq->IsInt() || seq->AsInt() < 1) return Fail(error, "falta \"sequence\"");

	Announce a;
	a.sequence = static_cast<std::uint64_t>(seq->AsInt());
	if (!StringList(root, "connectServers", kAnnounceMaxServers, a.connectServers, IsHostPort, error)) return false;
	if (const JsonValue* p = root.Find("patches"))
	{
		if (!p->IsObject()) return Fail(error, "\"patches\" tiene que ser un objeto");
		if (!OptString(*p, "channel", a.patchChannel, IsValidChannel, error)) return false;
		if (!StringList(*p, "urls", kAnnounceMaxUrls, a.patchUrls, IsValidUrl, error)) return false;
	}
	if (const JsonValue* b = root.Find("base"))
	{
		if (!b->IsObject()) return Fail(error, "\"base\" tiene que ser un objeto");
		if (!OptString(*b, "source", a.baseSource, IsBaseSource, error)) return false;
		if (!OptString(*b, "tag", a.baseTag, IsValidVersion, error)) return false;
		if (!OptString(*b, "key", a.baseKey, IsValidKeyHex, error)) return false;
	}
	if (!OptString(root, "news", a.news, IsValidNews, error)) return false;
	if (!OptString(root, "web", a.web, IsValidUrl, error)) return false;
	out = a;
	return true;
}

bool SameAnnounceContent(const Announce& a, const Announce& b)
{
	return a.connectServers == b.connectServers && a.patchChannel == b.patchChannel && a.patchUrls == b.patchUrls &&
	       a.baseSource == b.baseSource && a.baseTag == b.baseTag && a.baseKey == b.baseKey && a.news == b.news && a.web == b.web;
}

} // namespace Update
