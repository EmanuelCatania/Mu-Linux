#include "update/HttpTransport.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <mutex>

namespace Update {

namespace {

void GlobalInit()
{
	static std::once_flag once;
	std::call_once(once, [] {
		curl_global_init(CURL_GLOBAL_DEFAULT);
		std::atexit([] { curl_global_cleanup(); });
	});
}

struct Transfer
{
	CURL* curl = nullptr;
	bool ranged = false;
	std::uint64_t offset = 0;      // primer byte pedido
	std::uint64_t limit = 0;       // como mucho estos bytes
	std::uint64_t received = 0;
	bool started = false;
	std::uint64_t rangeStart = 0;  // de Content-Range
	bool haveRange = false;
	std::string why;               // motivo de un corte nuestro
	const DataSink* sink = nullptr;
	std::vector<std::uint8_t>* buffer = nullptr;
};

bool StartsWithNoCase(const char* s, std::size_t n, const char* prefix)
{
	const std::size_t p = std::strlen(prefix);
	if (n < p) return false;
	for (std::size_t i = 0; i < p; ++i)
	{
		char a = s[i], b = prefix[i];
		if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
		if (a != b) return false;
	}
	return true;
}

std::size_t OnHeader(char* data, std::size_t size, std::size_t count, void* user)
{
	Transfer& t = *static_cast<Transfer*>(user);
	const std::size_t n = size * count;
	// Una redirección trae sus propias cabeceras: se queda la última respuesta.
	if (StartsWithNoCase(data, n, "HTTP/")) t.haveRange = false;
	if (StartsWithNoCase(data, n, "content-range:"))
	{
		// "Content-Range: bytes 100-199/1000"
		const std::string line(data, n);
		const std::size_t at = line.find("bytes");
		if (at != std::string::npos)
		{
			t.rangeStart = std::strtoull(line.c_str() + at + 5, nullptr, 10);
			t.haveRange = true;
		}
	}
	return n;
}

std::size_t OnData(char* data, std::size_t size, std::size_t count, void* user)
{
	Transfer& t = *static_cast<Transfer*>(user);
	const std::size_t n = size * count;
	if (!t.started)
	{
		t.started = true;
		long code = 0;
		curl_easy_getinfo(t.curl, CURLINFO_RESPONSE_CODE, &code);
		if (t.ranged)
		{
			// 206 desde el byte pedido, o 200 sólo si se pidió desde el principio
			// (el server ignoró el Range pero los bytes coinciden).
			const bool okPartial = code == 206 && t.haveRange && t.rangeStart == t.offset;
			const bool okWhole = code == 200 && t.offset == 0;
			if (!okPartial && !okWhole)
			{
				t.why = code == 200 ? "el server no acepta pedidos por rango"
				                    : "respuesta HTTP " + std::to_string(code) + " a un pedido por rango";
				return 0;
			}
		}
		else if (code != 200)
		{
			t.why = "respuesta HTTP " + std::to_string(code);
			return 0;
		}
	}
	const std::uint64_t room = t.limit - t.received;
	if (n > room)
	{
		// Un 200 al pedido por rango: el archivo sigue más allá de lo pedido.
		if (t.ranged && t.offset == 0)
		{
			if (room && t.sink && !(*t.sink)(reinterpret_cast<const std::uint8_t*>(data), static_cast<std::size_t>(room)))
			{
				t.why = "cancelado";
				return 0;
			}
			t.received += room;
			t.why = "completo";
			return 0;
		}
		t.why = "llegó más de lo esperado";
		return 0;
	}
	if (t.sink && !(*t.sink)(reinterpret_cast<const std::uint8_t*>(data), n))
	{
		t.why = "cancelado";
		return 0;
	}
	if (t.buffer) t.buffer->insert(t.buffer->end(), data, data + n);
	t.received += n;
	return n;
}

CURL* NewHandle(const std::string& url, const HttpOptions& o, Transfer& t)
{
	CURL* c = curl_easy_init();
	if (!c) return nullptr;
	t.curl = c;
	curl_easy_setopt(c, CURLOPT_URL, url.c_str());
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(c, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
	// Desde https no se baja a http.
	curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, url.compare(0, 8, "https://") == 0 ? "https" : "http,https");
	curl_easy_setopt(c, CURLOPT_USERAGENT, o.userAgent.c_str());
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, o.connectTimeoutSec);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, o.lowSpeedBytes);
	curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, o.lowSpeedSec);
#ifdef _WIN32
	// Schannel: sin servidor de revocación accesible (proxies, redes cerradas)
	// no se corta la conexión; el certificado se sigue validando.
	curl_easy_setopt(c, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
	curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, OnHeader);
	curl_easy_setopt(c, CURLOPT_HEADERDATA, &t);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, OnData);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &t);
	return c;
}

bool Finish(CURL* c, CURLcode rc, Transfer& t, const std::string& url, std::string* error)
{
	const bool cutByUs = !t.why.empty();
	long code = 0;
	curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(c);
	// Una respuesta sin cuerpo no pasa por OnData: el código se mira acá también.
	if (rc == CURLE_OK && !t.started)
	{
		const bool ok = t.ranged ? (code == 206 || (code == 200 && t.offset == 0)) : code == 200;
		if (!ok)
		{
			if (error) *error = url + ": respuesta HTTP " + std::to_string(code);
			return false;
		}
	}
	if (rc == CURLE_OK || (cutByUs && t.why == "completo")) return true;
	if (error) *error = url + ": " + (cutByUs ? t.why : std::string(curl_easy_strerror(rc)));
	return false;
}

} // namespace

HttpTransport::HttpTransport(HttpOptions options) : m_Options(std::move(options)) { GlobalInit(); }

bool HttpTransport::Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
                          const DataSink& sink, std::string* error)
{
	if (!length) return true;
	Transfer t;
	t.ranged = true;
	t.offset = offset;
	t.limit = length;
	t.sink = &sink;
	CURL* c = NewHandle(url, m_Options, t);
	if (!c)
	{
		if (error) *error = "no se pudo iniciar libcurl";
		return false;
	}
	const std::string range = std::to_string(offset) + "-" + std::to_string(offset + length - 1);
	curl_easy_setopt(c, CURLOPT_RANGE, range.c_str());
	const CURLcode rc = curl_easy_perform(c);
	const std::uint64_t got = t.received;
	if (!Finish(c, rc, t, url, error)) return false;
	if (got != length)
	{
		if (error) *error = url + ": llegaron " + std::to_string(got) + " de " + std::to_string(length) + " bytes";
		return false;
	}
	return true;
}

bool HttpTransport::Get(const std::string& url, std::uint64_t maxSize, std::vector<std::uint8_t>& out,
                        std::string* error)
{
	out.clear();
	Transfer t;
	t.limit = maxSize;
	t.buffer = &out;
	CURL* c = NewHandle(url, m_Options, t);
	if (!c)
	{
		if (error) *error = "no se pudo iniciar libcurl";
		return false;
	}
	curl_easy_setopt(c, CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(maxSize));
	const CURLcode rc = curl_easy_perform(c);
	if (!Finish(c, rc, t, url, error))
	{
		out.clear();
		return false;
	}
	return true;
}

} // namespace Update
