// Parte pura de ServerTrust (Fase 5, C3): verificación de firmas y decisión de
// confianza.  Sin stdafx.h ni plataforma, para compilarla tal cual en las pruebas
// (tests/ServerTrust.cpp).

#include "net/ServerTrust.h"
#include <cstring>

extern "C" {
#include "net/crypto/monocypher.h"
}

namespace ServerTrust {

namespace {
// Igual que SERVER_IDENTITY_ROTATION_CONTEXT del server.
const char kRotationContext[] = "MuIdentityRotation1";

bool SignsNextKey(const std::uint8_t signer[KeySize], const std::uint8_t next[KeySize],
                  const std::uint8_t signature[SignatureSize])
{
    std::uint8_t message[sizeof(kRotationContext) - 1 + KeySize];
    std::memcpy(message, kRotationContext, sizeof(kRotationContext) - 1);
    std::memcpy(message + sizeof(kRotationContext) - 1, next, KeySize);
    return Verify(signature, signer, message, sizeof(message));
}

bool ParseHex(const char* text, std::size_t length, std::uint8_t* out, std::size_t size)
{
    if (length != size * 2) return false;
    for (std::size_t n = 0; n < length; ++n) {
        const char c = text[n];
        int digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        out[n / 2] = (std::uint8_t)(n % 2 ? out[n / 2] | digit : digit << 4);
    }
    return true;
}

void AppendHex(std::string& out, const std::uint8_t* bytes, std::size_t size)
{
    static const char digits[] = "0123456789abcdef";
    for (std::size_t n = 0; n < size; ++n) {
        out += digits[bytes[n] >> 4];
        out += digits[bytes[n] & 15];
    }
}
} // namespace

bool ParseKey(const char* text, std::uint8_t out[KeySize])
{
    return text && ParseHex(text, std::strlen(text), out, KeySize);
}

bool Verify(const std::uint8_t signature[SignatureSize],
            const std::uint8_t publicKey[KeySize],
            const std::uint8_t* message, std::size_t size)
{
    return crypto_eddsa_check(signature, publicKey, message, size) == 0;
}

Result Decide(bool hasPinned, const std::uint8_t pinned[KeySize],
              const std::uint8_t publicKey[KeySize],
              const std::uint8_t* previousKey,
              const std::uint8_t* rotationSignature)
{
    if (!hasPinned) return Result::FirstUse;
    if (crypto_verify32(pinned, publicKey) == 0) return Result::Trusted;
    // Otra clave: sólo vale si la fijada es la "anterior" y firmó a la nueva.
    if (!previousKey || !rotationSignature) return Result::Mismatch;
    if (crypto_verify32(pinned, previousKey) != 0) return Result::Mismatch;
    return SignsNextKey(previousKey, publicKey, rotationSignature) ? Result::Rotated : Result::Mismatch;
}

Result DecideFromRoot(const std::uint8_t root[KeySize], std::string& chain,
                      const std::uint8_t publicKey[KeySize],
                      const std::uint8_t* previousKey,
                      const std::uint8_t* rotationSignature)
{
    const std::size_t keyDigits = KeySize * 2, signatureDigits = SignatureSize * 2;
    std::uint8_t current[KeySize];
    std::memcpy(current, root, KeySize);
    std::string verified;
    for (std::size_t start = 0; start < chain.size();) {
        const std::size_t end = chain.find('\n', start);
        if (end == std::string::npos) break;   // línea sin terminar
        std::size_t length = end - start;
        if (length > 0 && chain[end - 1] == '\r') --length;
        if (length != keyDigits + 1 + signatureDigits) break;
        const char* line = chain.c_str() + start;
        std::uint8_t next[KeySize], signature[SignatureSize];
        if (line[keyDigits] != ' ' || !ParseHex(line, keyDigits, next, KeySize) ||
            !ParseHex(line + keyDigits + 1, signatureDigits, signature, SignatureSize) ||
            !SignsNextKey(current, next, signature))
            break;
        std::memcpy(current, next, KeySize);
        verified.append(line, length);
        verified += '\n';
        start = end + 1;
    }
    const Result result = Decide(true, current, publicKey, previousKey, rotationSignature);
    if (result == Result::Rotated) {
        AppendHex(verified, publicKey, KeySize);
        verified += ' ';
        AppendHex(verified, rotationSignature, SignatureSize);
        verified += '\n';
        chain = verified;
    }
    return result;
}

} // namespace ServerTrust
