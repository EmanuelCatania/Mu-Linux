#include "update/Signing.h"

#include "update/FileUtil.h"
#include "update/Hash.h"

#include "net/crypto/SecureRandom.h"

#include <cstring>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

extern "C" {
#include "net/crypto/monocypher.h"
}

namespace Update {

namespace {

void Derive(KeyPair& key)
{
	// crypto_eddsa_key_pair borra la semilla que recibe: se le pasa una copia.
	std::uint8_t seed[kKeySize];
	std::memcpy(seed, key.seed, sizeof(seed));
	crypto_eddsa_key_pair(key.secret, key.publicKey, seed);
}

std::vector<std::uint8_t> WithContext(const char* context, const void* data, std::size_t size)
{
	const std::size_t n = std::strlen(context);
	std::vector<std::uint8_t> message(n + size);
	std::memcpy(message.data(), context, n);
	if (size) std::memcpy(message.data() + n, data, size);
	return message;
}

std::string Trim(const std::string& s)
{
	std::size_t a = 0, b = s.size();
	while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
	return s.substr(a, b - a);
}

} // namespace

void KeyPair::Wipe()
{
	crypto_wipe(seed, sizeof(seed));
	crypto_wipe(secret, sizeof(secret));
}

bool GenerateKey(KeyPair& out)
{
	if (!SecureRandomBytes(out.seed, sizeof(out.seed))) return false;
	Derive(out);
	return true;
}

bool LoadKeyFile(const std::string& path, KeyPair& out, std::string* error)
{
	std::string text;
	if (!ReadWholeFile(path, text))
	{
		if (error) *error = "no se puede leer la clave: " + path;
		return false;
	}
	std::size_t start = 0;
	bool found = false;
	while (start <= text.size())
	{
		std::size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		const std::string line = Trim(text.substr(start, end - start));
		if (line.compare(0, 5, "seed=") == 0)
		{
			found = FromHex(Trim(line.substr(5)), out.seed, sizeof(out.seed));
			break;
		}
		start = end + 1;
	}
	crypto_wipe(&text[0], text.size());
	if (!found)
	{
		if (error) *error = "la clave no tiene una línea seed=<64 hex>: " + path;
		return false;
	}
	Derive(out);
	return true;
}

bool SaveKeyFile(const std::string& path, const KeyPair& key)
{
	std::string text;
	text += "# Clave de firma de parches (Mu-Linux). No la compartas: quien la tenga puede\n";
	text += "# publicar parches en tu nombre.  La publica, que si se comparte, es:\n";
	text += "# " + ToHex(key.publicKey, kKeySize) + "\n";
	text += "seed=" + ToHex(key.seed, kKeySize) + "\n";
#ifdef _WIN32
	const bool ok = WriteFileAtomic(path, text.data(), text.size());
#else
	// Se crea ya con 0600: escribirla y recién después cambiarle los permisos la
	// dejaría legible un instante.
	bool ok = false;
	const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd >= 0)
	{
		ok = ::fchmod(fd, 0600) == 0 && ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
		ok = ::close(fd) == 0 && ok;
	}
#endif
	crypto_wipe(&text[0], text.size());
	return ok;
}

bool ParsePublicKey(const std::string& hex, std::uint8_t out[kKeySize])
{
	return FromHex(Trim(hex), out, kKeySize);
}

void Sign(const char* context, const void* data, std::size_t size, const KeyPair& key,
          std::uint8_t signature[kSignatureSize])
{
	const std::vector<std::uint8_t> message = WithContext(context, data, size);
	crypto_eddsa_sign(signature, key.secret, message.data(), message.size());
}

bool Verify(const char* context, const void* data, std::size_t size,
            const std::uint8_t signature[kSignatureSize], const std::uint8_t publicKey[kKeySize])
{
	const std::vector<std::uint8_t> message = WithContext(context, data, size);
	return crypto_eddsa_check(signature, publicKey, message.data(), message.size()) == 0;
}

std::string FormatSignatureFile(const std::uint8_t signature[kSignatureSize])
{
	return ToHex(signature, kSignatureSize) + "\n";
}

bool ParseSignatureFile(const std::string& text, std::uint8_t signature[kSignatureSize])
{
	return FromHex(Trim(text), signature, kSignatureSize);
}

} // namespace Update
