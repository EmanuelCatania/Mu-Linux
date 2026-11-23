#pragma once

// Transporte HTTP del launcher (libcurl, TLS del sistema: Schannel en Windows,
// OpenSSL en Linux).  Va en su propio target (mu_update_http) para que mu-patch
// y el contenedor del server sigan compilando sin libcurl.
//
// - Un rango por pedido (mu-web contesta el archivo entero si se le piden
//   varios).  Si el server ignora el Range y responde 200 desde otro lugar que
//   el principio, el pedido falla en vez de mezclar bytes.
// - Sigue redirecciones (los assets de GitHub redirigen a otro host); desde
//   https sólo a https.
// - Sin compresión: los bytes llegan tal cual, que es lo que verifica el hash.
// - Corta una conexión que se queda casi quieta (lowSpeed*).  Los proxies del
//   sistema (variables https_proxy...) los toma libcurl.

#include "update/Transfer.h"

#include <string>

namespace Update {

struct HttpOptions
{
	std::string userAgent = "mu-launcher";
	long connectTimeoutSec = 15;
	long lowSpeedBytes = 512;     // menos que esto por segundo...
	long lowSpeedSec = 30;        // ...durante esto, y se corta
};

class HttpTransport : public Transport
{
public:
	explicit HttpTransport(HttpOptions options = HttpOptions());

	bool Fetch(const std::string& url, std::uint64_t offset, std::uint64_t length,
	           const DataSink& sink, std::string* error) override;
	bool Get(const std::string& url, std::uint64_t maxSize, std::vector<std::uint8_t>& out,
	         std::string* error) override;

private:
	HttpOptions m_Options;
};

} // namespace Update
