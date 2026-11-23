#pragma once

// Pide launcher.json al ConnectServer (F4:14/15, docs/parches-del-launcher.md).
//
// Abre la conexión, hace el saludo del canal cifrado (net/SecureChannel, el
// mismo del cliente) y pide el archivo de a pedazos.  Devuelve lo que manda el
// server tal cual: la firma (64 bytes) seguida de launcher.json.  Quien llama
// verifica la firma; acá sólo se decide si se confía en la identidad que
// presenta el server (trust), igual que el cliente.

#include "net/SecureChannel.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace Launcher {

class CsFetcher
{
public:
	virtual ~CsFetcher() = default;
	// identity: la clave con la que el server hizo el saludo (en la que se
	// confió).  false y el motivo en *error si no se pudo.
	virtual bool Fetch(const std::string& host, std::uint16_t port, const SecureChannel::TrustFn& trust,
	                   std::vector<std::uint8_t>& blob, std::uint8_t identity[SecureChannel::KeySize],
	                   std::string* error) = 0;
};

class SocketCsFetcher : public CsFetcher
{
public:
	// timeoutMs: para toda la conversación con un ConnectServer.
	explicit SocketCsFetcher(int timeoutMs = 5000, const std::atomic<bool>* cancel = nullptr)
		: m_TimeoutMs(timeoutMs), m_Cancel(cancel) {}
	bool Fetch(const std::string& host, std::uint16_t port, const SecureChannel::TrustFn& trust,
	           std::vector<std::uint8_t>& blob, std::uint8_t identity[SecureChannel::KeySize],
	           std::string* error) override;

private:
	int m_TimeoutMs;
	const std::atomic<bool>* m_Cancel;
};

} // namespace Launcher
