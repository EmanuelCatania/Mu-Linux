#include "update/FileUtil.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace Update {

namespace {

fs::path ToPath(const std::string& utf8)
{
#if defined(__cpp_char8_t)
	return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
	return fs::u8path(utf8);
#endif
}

std::string FromPath(const fs::path& p)
{
#if defined(__cpp_char8_t)
	const std::u8string s = p.generic_u8string();
	return std::string(s.begin(), s.end());
#else
	return p.generic_u8string();
#endif
}

FILE* OpenImpl(const std::string& path, const char* mode)
{
#ifdef _WIN32
	std::wstring wmode(mode, mode + std::char_traits<char>::length(mode));
	return _wfopen(ToPath(path).c_str(), wmode.c_str());
#else
	return std::fopen(path.c_str(), mode);
#endif
}

bool SeekImpl(FILE* f, std::uint64_t offset)
{
#ifdef _WIN32
	return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
	return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

template <class Container>
bool ReadAll(const std::string& path, Container& out)
{
	FILE* f = OpenImpl(path, "rb");
	if (!f) return false;
	out.clear();
	char buf[1 << 16];
	for (;;)
	{
		const std::size_t got = std::fread(buf, 1, sizeof(buf), f);
		out.insert(out.end(), buf, buf + got);
		if (got < sizeof(buf)) break;
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	return ok;
}

} // namespace

FILE* OpenFile(const std::string& path, const char* mode) { return OpenImpl(path, mode); }
bool SeekFile(FILE* f, std::uint64_t offset) { return SeekImpl(f, offset); }

bool ReadWholeFile(const std::string& path, std::vector<std::uint8_t>& out) { return ReadAll(path, out); }
bool ReadWholeFile(const std::string& path, std::string& out) { return ReadAll(path, out); }

bool ReadFileRange(const std::string& path, std::uint64_t offset, std::uint64_t size,
                   std::vector<std::uint8_t>& out)
{
	FILE* f = OpenImpl(path, "rb");
	if (!f) return false;
	bool ok = SeekImpl(f, offset);
	if (ok)
	{
		out.resize(static_cast<std::size_t>(size));
		ok = size == 0 || std::fread(out.data(), 1, out.size(), f) == out.size();
	}
	std::fclose(f);
	return ok;
}

bool MakeDirs(const std::string& path)
{
	if (path.empty()) return true;
	std::error_code ec;
	fs::create_directories(ToPath(path), ec);
	return !ec || fs::is_directory(ToPath(path), ec);
}

bool WriteFileAtomic(const std::string& path, const void* data, std::size_t size)
{
	const fs::path target = ToPath(path);
	if (target.has_parent_path() && !MakeDirs(FromPath(target.parent_path()))) return false;
	const std::string tmp = path + ".tmp";
	FILE* f = OpenImpl(tmp, "wb");
	if (!f) return false;
	bool ok = size == 0 || std::fwrite(data, 1, size, f) == size;
	ok = std::fflush(f) == 0 && ok;
	ok = std::fclose(f) == 0 && ok;
	std::error_code ec;
	if (ok) fs::rename(ToPath(tmp), target, ec);
	if (!ok || ec)
	{
		fs::remove(ToPath(tmp), ec);
		return false;
	}
	return true;
}

bool FileExists(const std::string& path)
{
	std::error_code ec;
	return fs::is_regular_file(ToPath(path), ec);
}

bool FileSize(const std::string& path, std::uint64_t& size)
{
	std::error_code ec;
	const auto s = fs::file_size(ToPath(path), ec);
	if (ec) return false;
	size = static_cast<std::uint64_t>(s);
	return true;
}

bool CopyFileTo(const std::string& from, const std::string& to)
{
	const fs::path target = ToPath(to);
	if (target.has_parent_path() && !MakeDirs(FromPath(target.parent_path()))) return false;
	std::error_code ec;
	fs::copy_file(ToPath(from), target, fs::copy_options::overwrite_existing, ec);
	return !ec;
}

bool FileModTime(const std::string& path, std::int64_t& out)
{
	std::error_code ec;
	const auto t = fs::last_write_time(ToPath(path), ec);
	if (ec) return false;
	out = static_cast<std::int64_t>(t.time_since_epoch().count());
	return true;
}

bool RemoveFile(const std::string& path)
{
	std::error_code ec;
	fs::remove(ToPath(path), ec);
	return !ec;
}

bool MoveFileOver(const std::string& from, const std::string& to, std::string* error)
{
	const fs::path target = ToPath(to);
	if (target.has_parent_path() && !MakeDirs(FromPath(target.parent_path())))
	{
		if (error) *error = "no se pudo crear la carpeta de " + to;
		return false;
	}
	std::error_code ec;
	fs::rename(ToPath(from), target, ec);
	if (!ec) return true;
	// Otro volumen: copiar al lado del destino y mover desde ahí.
	const fs::path tmp = ToPath(to + ".tmp");
	std::error_code ec2;
	fs::copy_file(ToPath(from), tmp, fs::copy_options::overwrite_existing, ec2);
	if (!ec2) fs::rename(tmp, target, ec2);
	if (!ec2)
	{
		fs::remove(ToPath(from), ec2);
		return true;
	}
	fs::remove(tmp, ec2);
	if (error) *error = "no se pudo reemplazar " + to + ": " + ec.message();
	return false;
}

std::string JoinPath(const std::string& dir, const std::string& rel)
{
	if (dir.empty()) return rel;
	const char last = dir.back();
	if (last == '/' || last == '\\') return dir + rel;
	return dir + "/" + rel;
}

bool ListFiles(const std::string& root, std::vector<std::string>& out, std::string* error)
{
	out.clear();
	std::error_code ec;
	const fs::path base = ToPath(root);
	if (!fs::is_directory(base, ec))
	{
		if (error) *error = "no es una carpeta: " + root;
		return false;
	}
	fs::recursive_directory_iterator it(base, fs::directory_options::none, ec), end;
	for (; !ec && it != end; it.increment(ec))
	{
		if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
		if (!it->is_regular_file(ec)) continue;
		out.push_back(FromPath(it->path().lexically_relative(base)));
	}
	if (ec)
	{
		if (error) *error = "no se pudo recorrer " + root + ": " + ec.message();
		return false;
	}
	std::sort(out.begin(), out.end());
	return true;
}

bool IsSafeRelativePath(const std::string& path)
{
	if (path.empty() || path.size() > 512 || path.front() == '/' || path.back() == '/') return false;
	std::size_t start = 0;
	for (;;)
	{
		const std::size_t slash = path.find('/', start);
		const std::string seg = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
		if (seg.empty() || seg == "." || seg == "..") return false;
		// Nombres reservados de Windows y finales que Windows recorta en silencio.
		if (seg.back() == '.' || seg.back() == ' ') return false;
		for (unsigned char c : seg)
			if (c < 0x20 || c == 0x7F || c == '\\' || c == ':' || c == '*' || c == '?' ||
			    c == '"' || c == '<' || c == '>' || c == '|') return false;
		std::string stem = seg.substr(0, seg.find('.'));
		std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
		static const char* const reserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4",
			"COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
		for (const char* r : reserved)
			if (stem == r) return false;
		if (slash == std::string::npos) return true;
		start = slash + 1;
	}
}

} // namespace Update
