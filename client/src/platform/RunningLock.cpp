// Ver RunningLock.h.  Sin stdafx.h: se compila igual en el cliente y en el launcher.

#include "platform/RunningLock.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <vector>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace RunningLock {

namespace {

#ifdef _WIN32
std::wstring Wide(const std::string& utf8)
{
	const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	std::vector<wchar_t> buf(n > 0 ? static_cast<std::size_t>(n) : 1, L'\0');
	if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, buf.data(), n);
	return std::wstring(buf.data());
}

HANDLE Open(const std::string& path)
{
	return CreateFileW(Wide(path).c_str(), GENERIC_READ | GENERIC_WRITE,
	                   FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
	                   FILE_ATTRIBUTE_NORMAL, nullptr);
}
#endif

} // namespace

bool Acquire(const std::string& path)
{
#ifdef _WIN32
	HANDLE h = Open(path);
	if (h == INVALID_HANDLE_VALUE) return false;
	OVERLAPPED ov{};
	if (!LockFileEx(h, 0, 0, 1, 0, &ov))   // compartido: varias instancias a la vez
	{
		CloseHandle(h);
		return false;
	}
	return true;   // el handle queda abierto hasta que termine el proceso
#else
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) return false;
	if (::flock(fd, LOCK_SH) != 0)
	{
		::close(fd);
		return false;
	}
	return true;
#endif
}

bool IsHeld(const std::string& path)
{
#ifdef _WIN32
	HANDLE h = Open(path);
	if (h == INVALID_HANDLE_VALUE) return false;
	OVERLAPPED ov{};
	const bool free = LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov) != 0;
	if (free) UnlockFileEx(h, 0, 1, 0, &ov);
	CloseHandle(h);
	return !free;
#else
	const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	if (fd < 0) return false;
	const bool free = ::flock(fd, LOCK_EX | LOCK_NB) == 0;
	::close(fd);   // suelta el lock exclusivo, si lo tomó
	return !free;
#endif
}

} // namespace RunningLock
