#include "update/Transfer.h"

#include "update/FileUtil.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace Update {

std::string FileUrlToPath(const std::string& url)
{
	static const char kPrefix[] = "file://";
	if (url.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return std::string();
	std::string rest = url.substr(sizeof(kPrefix) - 1);
	// file:///C:/x: sin la barra de delante de la letra de unidad.
	if (rest.size() >= 3 && rest[0] == '/' && rest[2] == ':') rest.erase(0, 1);
	return rest;
}

bool FileTransport::Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
                          const DataSink& sink, std::string* error)
{
	std::string path = FileUrlToPath(url);
	if (path.empty()) path = url;
	FILE* f = OpenFile(path, "rb");
	if (!f)
	{
		if (error) *error = "no existe " + url;
		return false;
	}
	bool ok = SeekFile(f, offset);
	std::vector<std::uint8_t> buf(1 << 16);
	std::uint64_t left = length;
	while (ok && left)
	{
		const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(left, buf.size()));
		const std::size_t got = std::fread(buf.data(), 1, want, f);
		if (got == 0) break;
		if (!sink(buf.data(), got))
		{
			ok = false;
			if (error) *error = "cancelado";
		}
		left -= got;
	}
	std::fclose(f);
	if (ok && left)
	{
		if (error) *error = url + ": el archivo es más corto que lo pedido";
		ok = false;
	}
	return ok;
}

RateLimiter::RateLimiter(std::uint64_t bytesPerSecond) : m_Last(Clock::now()) { SetRate(bytesPerSecond); }

void RateLimiter::SetRate(std::uint64_t bytesPerSecond)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		Refill(Clock::now());
		m_Rate = bytesPerSecond;
		// Un cuarto de segundo de ráfaga, con un mínimo para no cortar cada pedazo.
		m_Burst = std::max<double>(static_cast<double>(bytesPerSecond) / 4.0, 16.0 * 1024.0);
		m_Tokens = std::min(m_Tokens, m_Burst);
	}
	m_Cv.notify_all();
}

std::uint64_t RateLimiter::Rate() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Rate;
}

void RateLimiter::SetPaused(bool paused)
{
	{
		std::lock_guard<std::mutex> lock(m_Mutex);
		m_Paused = paused;
	}
	m_Cv.notify_all();
}

bool RateLimiter::Paused() const
{
	std::lock_guard<std::mutex> lock(m_Mutex);
	return m_Paused;
}

void RateLimiter::Wake() { m_Cv.notify_all(); }

void RateLimiter::Refill(Clock::time_point now)
{
	const double elapsed = std::chrono::duration<double>(now - m_Last).count();
	m_Last = now;
	if (m_Rate) m_Tokens = std::min(m_Burst, m_Tokens + elapsed * static_cast<double>(m_Rate));
}

bool RateLimiter::Acquire(std::size_t size, const std::atomic<bool>* cancel)
{
	std::unique_lock<std::mutex> lock(m_Mutex);
	double left = static_cast<double>(size);
	while (left > 0)
	{
		if (cancel && cancel->load()) return false;
		const Clock::time_point now = Clock::now();
		Refill(now);
		if (m_Paused)
		{
			m_Cv.wait_for(lock, std::chrono::milliseconds(100));
			continue;
		}
		if (!m_Rate) return true;
		const double take = std::min(left, m_Burst);
		if (m_Tokens >= take)
		{
			m_Tokens -= take;
			left -= take;
			continue;
		}
		const double wait = (take - m_Tokens) / static_cast<double>(m_Rate);
		m_Cv.wait_for(lock, std::chrono::duration<double>(std::min(wait, 0.1)));
	}
	return true;
}

} // namespace Update
