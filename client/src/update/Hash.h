#pragma once

// Hash de los archivos del actualizador: BLAKE2b de 32 bytes (el de monocypher,
// que ya usan el canal y la identidad del server).  En el manifiesto va como
// "blake2b-256:<64 hex>".

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace Update {

using Digest = std::array<std::uint8_t, 32>;

class Hasher
{
public:
	Hasher();
	void Update(const void* data, std::size_t size);
	Digest Final();

private:
	alignas(8) std::uint8_t m_Ctx[256];
};

Digest HashBytes(const void* data, std::size_t size);

// Hashea un archivo por partes (no lo carga entero).  false si no se puede leer.
bool HashFile(const std::string& path, Digest& out, std::uint64_t* size);

std::string ToHex(const std::uint8_t* data, std::size_t size);
// false si el texto no tiene exactamente 2*size dígitos hex.
bool FromHex(const std::string& text, std::uint8_t* out, std::size_t size);

std::string FormatDigest(const Digest& d);                      // "blake2b-256:..."
bool ParseDigest(const std::string& text, Digest& out);

} // namespace Update
