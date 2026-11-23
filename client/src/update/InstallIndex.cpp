#include "update/InstallIndex.h"

#include "update/FileUtil.h"
#include "update/Json.h"

#include <chrono>
#include <filesystem>

namespace Update {

namespace {

constexpr int kIndexFormat = 1;

// Margen para la granularidad de la fecha de los archivos (FAT: 2 s).
std::int64_t RacyMargin()
{
	return static_cast<std::int64_t>(
		std::chrono::duration_cast<std::filesystem::file_time_type::duration>(std::chrono::seconds(3)).count());
}

std::int64_t FileClockNow()
{
	return static_cast<std::int64_t>(std::filesystem::file_time_type::clock::now().time_since_epoch().count());
}

bool Fail(std::string* error, const std::string& why)
{
	if (error) *error = why;
	return false;
}

} // namespace

bool LoadIndex(const std::string& path, InstallIndex& out, std::string* error)
{
	out = InstallIndex();
	if (!FileExists(path)) return true;
	std::string text;
	if (!ReadWholeFile(path, text)) return Fail(error, "no se pudo leer " + path);
	JsonValue root;
	std::string why;
	if (!ParseJson(text, root, &why)) return Fail(error, path + ": " + why);
	const JsonValue* format = root.IsObject() ? root.Find("format") : nullptr;
	if (!format || !format->IsInt() || format->AsInt() != kIndexFormat) return Fail(error, path + ": formato desconocido");
	const JsonValue* revision = root.Find("revision");
	const JsonValue* savedAt = root.Find("savedAt");
	const JsonValue* files = root.Find("files");
	if (!revision || !revision->IsInt() || revision->AsInt() < 0 || !savedAt || !savedAt->IsInt() || !files || !files->IsArray())
		return Fail(error, path + ": falta revision, savedAt o files");
	out.revision = static_cast<std::uint64_t>(revision->AsInt());
	out.savedAt = savedAt->AsInt();
	for (const JsonValue& f : files->Items())
	{
		const JsonValue* p = f.IsObject() ? f.Find("path") : nullptr;
		const JsonValue* size = f.IsObject() ? f.Find("size") : nullptr;
		const JsonValue* mtime = f.IsObject() ? f.Find("mtime") : nullptr;
		const JsonValue* hash = f.IsObject() ? f.Find("hash") : nullptr;
		const JsonValue* channel = f.IsObject() ? f.Find("channel") : nullptr;
		if (!p || !p->IsString() || !IsSafeRelativePath(p->AsString()) || !size || !size->IsInt() || size->AsInt() < 0 ||
		    !mtime || !mtime->IsInt() || !hash || !hash->IsString() || !channel || !channel->IsString())
		{
			out = InstallIndex();
			return Fail(error, path + ": entrada inválida");
		}
		IndexEntry e;
		e.size = static_cast<std::uint64_t>(size->AsInt());
		e.mtime = mtime->AsInt();
		e.channel = channel->AsString();
		if (!ParseDigest(hash->AsString(), e.hash) || !out.files.emplace(p->AsString(), e).second)
		{
			out = InstallIndex();
			return Fail(error, path + ": hash inválido o ruta repetida");
		}
	}
	return true;
}

bool SaveIndex(const std::string& path, InstallIndex& index)
{
	index.savedAt = FileClockNow();
	JsonValue root = JsonValue::MakeObject();
	root.Set("format", JsonValue::MakeInt(kIndexFormat));
	root.Set("revision", JsonValue::MakeInt(static_cast<std::int64_t>(index.revision)));
	root.Set("savedAt", JsonValue::MakeInt(index.savedAt));
	JsonValue files = JsonValue::MakeArray();
	for (const auto& [rel, e] : index.files)
	{
		JsonValue f = JsonValue::MakeObject();
		f.Set("path", JsonValue::MakeString(rel));
		f.Set("size", JsonValue::MakeInt(static_cast<std::int64_t>(e.size)));
		f.Set("mtime", JsonValue::MakeInt(e.mtime));
		f.Set("hash", JsonValue::MakeString(FormatDigest(e.hash)));
		f.Set("channel", JsonValue::MakeString(e.channel));
		files.Push(std::move(f));
	}
	root.Set("files", std::move(files));
	const std::string text = root.Dump();
	return WriteFileAtomic(path, text.data(), text.size());
}

LocalState CheckLocal(const std::string& root, const std::string& rel, const Digest& expected,
                      InstallIndex& index, bool fullVerify, Digest* actual)
{
	const std::string full = JoinPath(root, rel);
	std::uint64_t size = 0;
	std::int64_t mtime = 0;
	if (!FileSize(full, size))
	{
		if (!FileExists(full)) return LocalState::Missing;
		return LocalState::Unreadable;
	}
	if (!FileModTime(full, mtime)) return LocalState::Unreadable;

	auto it = index.files.find(rel);
	const bool trusted = index.savedAt != 0 && mtime < index.savedAt - RacyMargin();
	if (!fullVerify && trusted && it != index.files.end() && it->second.size == size && it->second.mtime == mtime)
	{
		if (actual) *actual = it->second.hash;
		return it->second.hash == expected ? LocalState::Match : LocalState::Differs;
	}

	Digest hash{};
	std::uint64_t hashed = 0;
	if (!HashFile(full, hash, &hashed)) return LocalState::Unreadable;
	if (actual) *actual = hash;
	IndexEntry& e = index.files[rel];
	e.size = hashed;
	e.mtime = mtime;
	e.hash = hash;
	return hash == expected ? LocalState::Match : LocalState::Differs;
}

} // namespace Update
