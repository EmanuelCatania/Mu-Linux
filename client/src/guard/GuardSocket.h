#pragma once

// Conexión TCP bloqueante con el canal cifrado (net/SecureChannel), para el
// guardián de MU-Guard: la consulta corta al ConnectServer (F4:16) y la conexión
// con mu-guard.  No usa el socket ni los globals del juego: el guardián corre
// antes de que exista la ventana.

#include "net/SecureChannel.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Guard {

class SecureSocket
{
public:
	SecureSocket() = default;
	~SecureSocket() { this->Close(); }
	SecureSocket(const SecureSocket&) = delete;
	SecureSocket& operator=(const SecureSocket&) = delete;

	// Conecta y termina el saludo.  false si no se pudo (no contesta, no habla el
	// canal o la identidad no es de confianza).
	bool Open(const std::string& host, std::uint16_t port, int timeoutMs, const SecureChannel::TrustFn& trust);

	bool IsOpen() const { return this->m_Socket != kNone; }

	// Manda `size` bytes en un registro.
	bool Send(const void* data, std::size_t size, int timeoutMs);

	// Espera texto claro hasta el plazo y lo agrega a `plain`.  false si se
	// cortó, falló el canal o venció el plazo sin que llegara nada.
	bool Receive(std::vector<std::uint8_t>& plain, int timeoutMs);

	void Close();

private:
	static constexpr std::intptr_t kNone = -1;

	std::intptr_t m_Socket = kNone;

	SecureChannel m_Channel;
};

} // namespace Guard
