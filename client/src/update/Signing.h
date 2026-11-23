#pragma once

// Firma de manifiestos y configuraciones del launcher.  EdDSA de monocypher, el
// mismo esquema de la identidad del server: el overlay de un server se firma con
// su Data/Keys/ServerIdentity.key, que el cliente ya tiene anclada.
//
// Lo firmado lleva un contexto propio delante (MuPatch1, MuLauncher1) para que
// una firma de un manifiesto no valga como firma del saludo del canal
// (MuHandshake1) ni al revés, aunque usen la misma clave.

#include <cstddef>
#include <cstdint>
#include <string>

namespace Update {

constexpr char kPatchContext[] = "MuPatch1";
constexpr char kLauncherContext[] = "MuLauncher1";

constexpr std::size_t kKeySize = 32;
constexpr std::size_t kSignatureSize = 64;

struct KeyPair
{
	std::uint8_t seed[kKeySize];
	std::uint8_t secret[64];
	std::uint8_t publicKey[kKeySize];

	void Wipe();
};

bool GenerateKey(KeyPair& out);

// Lee un archivo con una línea "seed=<64 hex>", el formato de
// Data/Keys/ServerIdentity.key.  Las demás líneas se ignoran.
bool LoadKeyFile(const std::string& path, KeyPair& out, std::string* error);
// Lo escribe en ese mismo formato (en POSIX, sólo legible por el dueño).
bool SaveKeyFile(const std::string& path, const KeyPair& key);

// Clave pública: 64 dígitos hex.
bool ParsePublicKey(const std::string& hex, std::uint8_t out[kKeySize]);

void Sign(const char* context, const void* data, std::size_t size, const KeyPair& key,
          std::uint8_t signature[kSignatureSize]);
bool Verify(const char* context, const void* data, std::size_t size,
            const std::uint8_t signature[kSignatureSize], const std::uint8_t publicKey[kKeySize]);

// Archivo .sig: la firma en hex y un salto de línea.
std::string FormatSignatureFile(const std::uint8_t signature[kSignatureSize]);
bool ParseSignatureFile(const std::string& text, std::uint8_t signature[kSignatureSize]);

} // namespace Update
