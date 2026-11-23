#include "update/ChannelFetch.h"

#include "update/FileUtil.h"
#include "update/Json.h"

#include <algorithm>

namespace Update {

namespace {

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

std::string Join(const std::string& base, const std::string& name)
{
	if (base.empty()) return name;
	const char last = base.back();
	return (last == '/' || last == '\\') ? base + name : base + "/" + name;
}

// Valida un manifiesto ya bajado.  false con *fatal = true si el problema no se
// arregla probando otra URL (todas sirven lo mismo, firmado por el mismo).
bool Accept(const ChannelSpec& spec, const SequenceStore& sequences, const std::vector<std::uint8_t>& text,
            const std::vector<std::uint8_t>& sigText, FetchedChannel& out, std::string* error)
{
	std::uint8_t sig[kSignatureSize];
	if (!ParseSignatureFile(std::string(sigText.begin(), sigText.end()), sig))
		return Fail(error, "la firma del manifiesto no tiene formato válido");
	if (!Verify(kPatchContext, text.data(), text.size(), sig, spec.publicKey))
		return Fail(error, "la firma del manifiesto no es de la clave esperada");
	std::string why;
	bool unknown = false;
	if (!ParseManifest(std::string(text.begin(), text.end()), out.manifest, &why, &unknown))
	{
		out.unknownFormat = unknown;
		return Fail(error, unknown ? "el manifiesto es de un formato más nuevo: hace falta actualizar el launcher" : why);
	}
	if (out.manifest.channel != spec.channel)
		return Fail(error, "se pidió el canal " + spec.channel + " y llegó " + out.manifest.channel);
	if (!spec.version.empty() && out.manifest.version != spec.version)
		return Fail(error, spec.channel + ": se pidió la versión " + spec.version + " y llegó la " + out.manifest.version);
	const std::uint64_t seen = sequences.Seen(spec.channel);
	if (out.manifest.sequence < seen)
		return Fail(error, spec.channel + ": la secuencia " + std::to_string(out.manifest.sequence) +
		                       " es menor que la ya vista (" + std::to_string(seen) + "); se rechaza un parche viejo");
	return true;
}

} // namespace

bool SequenceStore::Load(const std::string& path, std::string* error)
{
	m_Seen.clear();
	if (!FileExists(path)) return true;
	std::string text;
	if (!ReadWholeFile(path, text)) return Fail(error, "no se pudo leer " + path);
	JsonValue root;
	std::string why;
	if (!ParseJson(text, root, &why)) return Fail(error, path + ": " + why);
	const JsonValue* channels = root.IsObject() ? root.Find("channels") : nullptr;
	if (!channels || !channels->IsObject()) return Fail(error, path + ": falta channels");
	for (const auto& [name, v] : channels->Members())
	{
		if (!IsValidChannel(name) || !v.IsInt() || v.AsInt() < 0)
		{
			m_Seen.clear();
			return Fail(error, path + ": entrada inválida");
		}
		m_Seen[name] = static_cast<std::uint64_t>(v.AsInt());
	}
	return true;
}

bool SequenceStore::Save(const std::string& path) const
{
	JsonValue root = JsonValue::MakeObject();
	JsonValue channels = JsonValue::MakeObject();
	for (const auto& [name, seq] : m_Seen) channels.Set(name, JsonValue::MakeInt(static_cast<std::int64_t>(seq)));
	root.Set("channels", std::move(channels));
	const std::string text = root.Dump();
	return WriteFileAtomic(path, text.data(), text.size());
}

std::uint64_t SequenceStore::Seen(const std::string& channel) const
{
	const auto it = m_Seen.find(channel);
	return it == m_Seen.end() ? 0 : it->second;
}

void SequenceStore::Record(const std::string& channel, std::uint64_t sequence)
{
	std::uint64_t& s = m_Seen[channel];
	s = std::max(s, sequence);
}

bool FetchChannel(Transport& transport, const ChannelSpec& spec, SequenceStore& sequences,
                  FetchedChannel& out, std::string* error)
{
	out = FetchedChannel();
	if (spec.urls.empty()) return Fail(error, spec.channel + ": no hay de dónde bajarlo");
	std::string last;
	for (std::size_t i = 0; i < spec.urls.size(); ++i)
	{
		const std::string& base = spec.urls[i];
		std::vector<std::uint8_t> text, sig;
		std::string why;
		if (!transport.Get(Join(base, spec.manifestName), kMaxManifestSize, text, &why) ||
		    !transport.Get(Join(base, spec.manifestName + ".sig"), 4096, sig, &why))
		{
			last = why;
			continue;
		}
		FetchedChannel candidate;
		if (!Accept(spec, sequences, text, sig, candidate, &why))
		{
			// Otra URL podría servir otra cosa (un espejo desactualizado), pero un
			// formato nuevo vale para todas.
			last = why;
			if (candidate.unknownFormat)
			{
				out.unknownFormat = true;
				break;
			}
			continue;
		}
		out = std::move(candidate);
		out.urls.push_back(base);
		for (std::size_t j = 0; j < spec.urls.size(); ++j)
			if (j != i) out.urls.push_back(spec.urls[j]);
		sequences.Record(spec.channel, out.manifest.sequence);
		return true;
	}
	return Fail(error, spec.channel + ": " + last);
}

ChannelSource SourceOf(const FetchedChannel& fetched, int priority)
{
	ChannelSource s;
	s.manifest = &fetched.manifest;
	s.baseUrls = fetched.urls;
	s.priority = priority;
	return s;
}

std::string GithubReleaseUrl(const std::string& repo, const std::string& tag)
{
	return "https://github.com/" + repo + "/releases/download/" + tag;
}

std::string ReleaseManifestName(const std::string& channel)
{
	std::string slug = channel;
	std::replace(slug.begin(), slug.end(), '/', '-');
	return slug + ".manifest.json";
}

bool CheckRequires(const Manifest& base, const Manifest& overlay, int protocol, std::string* error)
{
	if (!overlay.requiresBase.empty() && overlay.requiresBase != base.version)
		return Fail(error, "el server pide la base " + overlay.requiresBase + " y se bajó la " + base.version);
	if (overlay.requiresProtocol && protocol && overlay.requiresProtocol != protocol)
		return Fail(error, "el server pide el protocolo " + std::to_string(overlay.requiresProtocol) +
		                       " y este launcher es del " + std::to_string(protocol));
	return true;
}

} // namespace Update
