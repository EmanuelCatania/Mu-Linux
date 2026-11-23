#include "update/Manifest.h"

#include "update/FileUtil.h"
#include "update/Json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <utility>

namespace Update {

namespace {

// Tope de un archivo y de un zip: 1 TB.  Sólo evita números absurdos.
constexpr std::int64_t kMaxSize = 1ll << 40;

JsonValue Str(const std::string& s) { return JsonValue::MakeString(s); }
JsonValue Int(std::uint64_t v) { return JsonValue::MakeInt(static_cast<std::int64_t>(v)); }

bool Fail(std::string* error, const std::string& what)
{
	if (error) *error = what;
	return false;
}

bool GetString(const JsonValue& o, const char* key, std::string& out, bool required, std::string* error)
{
	const JsonValue* v = o.Find(key);
	if (!v) return required ? Fail(error, std::string("falta \"") + key + "\"") : true;
	if (!v->IsString()) return Fail(error, std::string("\"") + key + "\" tiene que ser un string");
	out = v->AsString();
	return true;
}

bool GetInt(const JsonValue& o, const char* key, std::int64_t& out, std::int64_t lo, std::int64_t hi,
            bool required, std::string* error)
{
	const JsonValue* v = o.Find(key);
	if (!v) return required ? Fail(error, std::string("falta \"") + key + "\"") : true;
	if (!v->IsInt() || v->AsInt() < lo || v->AsInt() > hi)
		return Fail(error, std::string("\"") + key + "\" fuera de rango");
	out = v->AsInt();
	return true;
}

bool GetBool(const JsonValue& o, const char* key, bool& out, std::string* error)
{
	const JsonValue* v = o.Find(key);
	if (!v) return true;
	if (!v->IsBool()) return Fail(error, std::string("\"") + key + "\" tiene que ser true o false");
	out = v->AsBool();
	return true;
}

bool GetDigest(const JsonValue& o, const char* key, Digest& out, std::string* error)
{
	std::string s;
	if (!GetString(o, key, s, true, error)) return false;
	if (!ParseDigest(s, out)) return Fail(error, std::string("\"") + key + "\" no es blake2b-256:<64 hex>");
	return true;
}

bool ParseMethod(const std::string& s, ZipMethod& out)
{
	if (s == "store") out = ZipMethod::Store;
	else if (s == "zstd") out = ZipMethod::Zstd;
	else if (s == "deflate") out = ZipMethod::Deflate;
	else return false;
	return true;
}

std::string Lower(const std::string& s)
{
	std::string r = s;
	std::transform(r.begin(), r.end(), r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return r;
}

bool IsBundleName(const std::string& n)
{
	if (n.size() < 5 || n.size() > 128 || n.compare(n.size() - 4, 4, ".zip") != 0) return false;
	for (unsigned char c : n)
		if (!std::isalnum(c) && c != '.' && c != '-' && c != '_') return false;
	return n[0] != '.';
}

// Carpetas del cliente que el launcher no toca nunca: ahí están las opciones y
// la caché del jugador.
bool IsReservedPath(const std::string& path)
{
	const std::string l = Lower(path);
	return l.compare(0, 5, "user/") == 0;
}

bool ValidPath(const std::string& path)
{
	return IsSafeRelativePath(path) && !IsReservedPath(path);
}

} // namespace

const char* MethodName(ZipMethod m)
{
	switch (m)
	{
	case ZipMethod::Store: return "store";
	case ZipMethod::Deflate: return "deflate";
	case ZipMethod::Zstd: return "zstd";
	}
	return "store";
}

bool IsValidVersion(const std::string& v)
{
	if (v.empty() || v.size() > 32) return false;
	int parts = 0;
	std::size_t i = 0;
	while (i < v.size())
	{
		const std::size_t start = i;
		while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) ++i;
		if (i == start || i - start > 6) return false;
		++parts;
		if (i == v.size()) break;
		if (v[i] != '.') return false;
		++i;
		if (i == v.size()) return false;
	}
	return parts >= 2 && parts <= 4;
}

int CompareVersions(const std::string& a, const std::string& b)
{
	std::size_t i = 0, j = 0;
	while (i < a.size() || j < b.size())
	{
		long x = 0, y = 0;
		while (i < a.size() && a[i] != '.') x = x * 10 + (a[i++] - '0');
		while (j < b.size() && b[j] != '.') y = y * 10 + (b[j++] - '0');
		if (x != y) return x < y ? -1 : 1;
		if (i < a.size()) ++i;
		if (j < b.size()) ++j;
	}
	return 0;
}

bool IsValidChannel(const std::string& c)
{
	if (c.empty() || c.size() > 64) return false;
	int slashes = 0;
	char prev = '/';
	for (unsigned char ch : c)
	{
		if (ch == '/')
		{
			if (prev == '/' || ++slashes > 1) return false;
		}
		else if (!(std::islower(ch) || std::isdigit(ch) || ch == '-' || ch == '_' || ch == '.'))
		{
			return false;
		}
		prev = static_cast<char>(ch);
	}
	return slashes == 1 && prev != '/';
}

bool IsValidPlatform(const std::string& p)
{
	if (p.empty() || p.size() > 32) return false;
	for (unsigned char c : p)
		if (!(std::islower(c) || std::isdigit(c) || c == '-' || c == '_')) return false;
	return true;
}

std::string DumpManifest(const Manifest& m)
{
	JsonValue root = JsonValue::MakeObject();
	root.Set("format", JsonValue::MakeInt(m.format));
	root.Set("channel", Str(m.channel));
	root.Set("version", Str(m.version));
	root.Set("sequence", Int(m.sequence));
	JsonValue req = JsonValue::MakeObject();
	if (!m.requiresBase.empty()) req.Set("base", Str(m.requiresBase));
	if (m.requiresProtocol) req.Set("protocol", JsonValue::MakeInt(m.requiresProtocol));
	root.Set("requires", std::move(req));
	JsonValue platforms = JsonValue::MakeArray();
	for (const auto& p : m.platforms) platforms.Push(Str(p));
	root.Set("platforms", std::move(platforms));

	JsonValue bundles = JsonValue::MakeArray();
	for (const BundleRef& b : m.bundles)
	{
		JsonValue o = JsonValue::MakeObject();
		o.Set("name", Str(b.name));
		o.Set("size", Int(b.size));
		o.Set("hash", Str(FormatDigest(b.hash)));
		if (!b.url.empty()) o.Set("url", Str(b.url));
		bundles.Push(std::move(o));
	}
	root.Set("bundles", std::move(bundles));

	JsonValue files = JsonValue::MakeArray();
	for (const FileEntry& f : m.files)
	{
		JsonValue o = JsonValue::MakeObject();
		o.Set("path", Str(f.path));
		o.Set("size", Int(f.size));
		o.Set("hash", Str(FormatDigest(f.hash)));
		o.Set("bundle", Int(f.bundle));
		o.Set("offset", Int(f.offset));
		o.Set("packed", Int(f.packed));
		o.Set("method", Str(MethodName(f.method)));
		o.Set("required", JsonValue::MakeBool(f.required));
		if (!f.platform.empty()) o.Set("platform", Str(f.platform));
		if (f.self) o.Set("self", JsonValue::MakeBool(true));
		if (!f.deltas.empty())
		{
			JsonValue deltas = JsonValue::MakeArray();
			for (const DeltaRef& d : f.deltas)
			{
				JsonValue x = JsonValue::MakeObject();
				x.Set("from", Str(FormatDigest(d.from)));
				x.Set("bundle", Int(d.bundle));
				x.Set("offset", Int(d.offset));
				x.Set("packed", Int(d.packed));
				deltas.Push(std::move(x));
			}
			o.Set("deltas", std::move(deltas));
		}
		files.Push(std::move(o));
	}
	root.Set("files", std::move(files));

	if (!m.deleted.empty())
	{
		JsonValue deleted = JsonValue::MakeArray();
		for (const auto& d : m.deleted) deleted.Push(Str(d));
		root.Set("deleted", std::move(deleted));
	}
	return root.Dump();
}

bool ParseManifest(const std::string& text, Manifest& out, std::string* error, bool* unknownFormat)
{
	if (unknownFormat) *unknownFormat = false;
	JsonValue root;
	std::string jsonError;
	if (!ParseJson(text, root, &jsonError)) return Fail(error, "manifest.json no es JSON válido: " + jsonError);
	if (!root.IsObject()) return Fail(error, "manifest.json tiene que ser un objeto");

	Manifest m;
	std::int64_t v = 0;
	if (!GetInt(root, "format", v, 1, 1000, true, error)) return false;
	m.format = static_cast<int>(v);
	if (m.format != kManifestFormat)
	{
		if (unknownFormat) *unknownFormat = true;
		return Fail(error, "formato de manifiesto desconocido: " + std::to_string(m.format));
	}
	if (!GetString(root, "channel", m.channel, true, error)) return false;
	if (!IsValidChannel(m.channel)) return Fail(error, "canal no válido: " + m.channel);
	if (!GetString(root, "version", m.version, true, error)) return false;
	if (!IsValidVersion(m.version)) return Fail(error, "versión no válida: " + m.version);
	if (!GetInt(root, "sequence", v, 1, INT64_MAX, true, error)) return false;
	m.sequence = static_cast<std::uint64_t>(v);

	if (const JsonValue* req = root.Find("requires"))
	{
		if (!req->IsObject()) return Fail(error, "\"requires\" tiene que ser un objeto");
		if (!GetString(*req, "base", m.requiresBase, false, error)) return false;
		if (!m.requiresBase.empty() && !IsValidVersion(m.requiresBase))
			return Fail(error, "requires.base no válido: " + m.requiresBase);
		v = 0;
		if (!GetInt(*req, "protocol", v, 0, 65535, false, error)) return false;
		m.requiresProtocol = static_cast<int>(v);
	}

	if (const JsonValue* platforms = root.Find("platforms"))
	{
		if (!platforms->IsArray()) return Fail(error, "\"platforms\" tiene que ser un arreglo");
		for (const JsonValue& p : platforms->Items())
		{
			if (!p.IsString() || !IsValidPlatform(p.AsString())) return Fail(error, "plataforma no válida");
			m.platforms.push_back(p.AsString());
		}
	}

	const JsonValue* bundles = root.Find("bundles");
	if (!bundles || !bundles->IsArray()) return Fail(error, "falta el arreglo \"bundles\"");
	std::set<std::string> bundleNames;
	for (const JsonValue& b : bundles->Items())
	{
		if (!b.IsObject()) return Fail(error, "bundle no válido");
		BundleRef r;
		if (!GetString(b, "name", r.name, true, error)) return false;
		if (!IsBundleName(r.name)) return Fail(error, "nombre de bundle no válido: " + r.name);
		if (!bundleNames.insert(Lower(r.name)).second) return Fail(error, "bundle repetido: " + r.name);
		if (!GetInt(b, "size", v, 22, kMaxSize, true, error)) return false;
		r.size = static_cast<std::uint64_t>(v);
		if (!GetDigest(b, "hash", r.hash, error)) return false;
		if (!GetString(b, "url", r.url, false, error)) return false;
		if (!r.url.empty() && r.url.compare(0, 8, "https://") != 0 && r.url.compare(0, 7, "http://") != 0)
			return Fail(error, "url de bundle no válida: " + r.url);
		m.bundles.push_back(std::move(r));
	}

	auto checkRange = [&](std::uint32_t bundle, std::uint64_t offset, std::uint64_t packed, const std::string& what) {
		if (bundle >= m.bundles.size()) return Fail(error, "bundle fuera de rango en " + what);
		const std::uint64_t size = m.bundles[bundle].size;
		if (offset > size || packed > size - offset) return Fail(error, "tramo fuera del zip en " + what);
		return true;
	};

	const JsonValue* files = root.Find("files");
	if (!files || !files->IsArray()) return Fail(error, "falta el arreglo \"files\"");
	std::set<std::pair<std::string, std::string>> seen;
	for (const JsonValue& o : files->Items())
	{
		if (!o.IsObject()) return Fail(error, "archivo no válido");
		FileEntry f;
		if (!GetString(o, "path", f.path, true, error)) return false;
		if (!ValidPath(f.path)) return Fail(error, "ruta no permitida: " + f.path);
		if (!GetInt(o, "size", v, 0, kMaxSize, true, error)) return false;
		f.size = static_cast<std::uint64_t>(v);
		if (!GetDigest(o, "hash", f.hash, error)) return false;
		if (!GetInt(o, "bundle", v, 0, UINT32_MAX, true, error)) return false;
		f.bundle = static_cast<std::uint32_t>(v);
		if (!GetInt(o, "offset", v, 0, kMaxSize, true, error)) return false;
		f.offset = static_cast<std::uint64_t>(v);
		if (!GetInt(o, "packed", v, 0, kMaxSize, true, error)) return false;
		f.packed = static_cast<std::uint64_t>(v);
		std::string method;
		if (!GetString(o, "method", method, true, error)) return false;
		if (!ParseMethod(method, f.method)) return Fail(error, "método no válido en " + f.path + ": " + method);
		if (f.method == ZipMethod::Store && f.packed != f.size) return Fail(error, "tamaños distintos en " + f.path);
		if (!GetBool(o, "required", f.required, error)) return false;
		if (!GetString(o, "platform", f.platform, false, error)) return false;
		if (!f.platform.empty() && !IsValidPlatform(f.platform)) return Fail(error, "plataforma no válida en " + f.path);
		if (!GetBool(o, "self", f.self, error)) return false;
		if (!checkRange(f.bundle, f.offset, f.packed, f.path)) return false;
		if (const JsonValue* deltas = o.Find("deltas"))
		{
			if (!deltas->IsArray()) return Fail(error, "\"deltas\" no válido en " + f.path);
			for (const JsonValue& x : deltas->Items())
			{
				if (!x.IsObject()) return Fail(error, "delta no válido en " + f.path);
				DeltaRef d;
				if (!GetDigest(x, "from", d.from, error)) return false;
				if (!GetInt(x, "bundle", v, 0, UINT32_MAX, true, error)) return false;
				d.bundle = static_cast<std::uint32_t>(v);
				if (!GetInt(x, "offset", v, 0, kMaxSize, true, error)) return false;
				d.offset = static_cast<std::uint64_t>(v);
				if (!GetInt(x, "packed", v, 1, kMaxSize, true, error)) return false;
				d.packed = static_cast<std::uint64_t>(v);
				if (!checkRange(d.bundle, d.offset, d.packed, f.path + " (delta)")) return false;
				f.deltas.push_back(d);
			}
		}
		// En Windows no se distinguen mayúsculas: dos rutas que sólo difieren en
		// eso serían el mismo archivo.
		if (!seen.insert({ f.platform, Lower(f.path) }).second) return Fail(error, "ruta repetida: " + f.path);
		m.files.push_back(std::move(f));
	}
	// Una ruta para todas las plataformas no puede convivir con la misma ruta
	// para una plataforma concreta.
	for (const FileEntry& f : m.files)
		if (!f.platform.empty() && seen.count({ std::string(), Lower(f.path) }))
			return Fail(error, "ruta repetida: " + f.path);

	if (const JsonValue* deleted = root.Find("deleted"))
	{
		if (!deleted->IsArray()) return Fail(error, "\"deleted\" tiene que ser un arreglo");
		for (const JsonValue& d : deleted->Items())
		{
			if (!d.IsString() || !ValidPath(d.AsString())) return Fail(error, "ruta a borrar no permitida");
			for (const FileEntry& f : m.files)
				if (Lower(f.path) == Lower(d.AsString())) return Fail(error, "se borra y se instala a la vez: " + f.path);
			m.deleted.push_back(d.AsString());
		}
	}

	out = std::move(m);
	return true;
}

} // namespace Update
