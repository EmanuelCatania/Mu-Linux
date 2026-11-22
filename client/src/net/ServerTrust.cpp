#include "stdafx.h"
#include "net/ServerTrust.h"
#include "app/config/ServerConfig.h"
#include "platform/IFileSystem.h"
#include "platform/UserPath.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

// Lo que se guarda de cada server: la cadena de rotaciones (con clave
// compilada) o la clave del primer contacto (sin ella).  La decisión y la
// verificación de firmas están en ServerTrustCore.cpp.

extern "C" void DbgLogPublic(const char* msg);

namespace ServerTrust {

namespace {

std::string s_Dir;

std::string PinFile() { return s_Dir + "/server.pub"; }

bool ReadPin(std::uint8_t out[KeySize])
{
    if (!s_Dir.empty()) {
        if (FILE* f = Platform::GetFileSystem().Open(PinFile().c_str(), "rb")) {
            char text[KeySize * 2 + 1] = {};
            const std::size_t got = fread(text, 1, KeySize * 2, f);
            fclose(f);
            if (got == KeySize * 2 && ParseKey(text, out)) return true;
        }
    }
    return false;
}

void WritePin(const std::uint8_t key[KeySize])
{
    if (s_Dir.empty()) return;
    Platform::GetFileSystem().EnsureDirectory(s_Dir.c_str());
    FILE* f = Platform::GetFileSystem().Open(PinFile().c_str(), "wb");
    if (!f) return;
    static const char digits[] = "0123456789abcdef";
    char text[KeySize * 2];
    for (std::size_t n = 0; n < KeySize; ++n) {
        text[n * 2] = digits[key[n] >> 4];
        text[n * 2 + 1] = digits[key[n] & 15];
    }
    fwrite(text, 1, sizeof(text), f);
    fclose(f);
}

// User/Cache/server-<primeros 16 dígitos de la raíz>.chain
std::string ChainFile(const std::uint8_t root[KeySize])
{
    std::string path = std::string(Platform::UserDir()) + "Cache/server-";
    static const char digits[] = "0123456789abcdef";
    for (std::size_t n = 0; n < 8; ++n) {
        path += digits[root[n] >> 4];
        path += digits[root[n] & 15];
    }
    return path + ".chain";
}

std::string ReadChain(const std::string& path)
{
    std::string text;
    if (FILE* f = Platform::GetFileSystem().Open(path.c_str(), "rb")) {
        char part[1024];
        std::size_t got;
        while (text.size() < 0x10000 && (got = fread(part, 1, sizeof(part), f)) > 0) text.append(part, got);
        fclose(f);
    }
    return text;
}

// Temporal y reemplazo: una cadena cortada deja afuera al cliente en la
// rotación siguiente.
void WriteChain(const std::string& path, const std::string& text)
{
    Platform::GetFileSystem().EnsureDirectory((std::string(Platform::UserDir()) + "Cache").c_str());
    const std::string temp = path + ".tmp";
    FILE* f = Platform::GetFileSystem().Open(temp.c_str(), "wb");
    bool ok = f != nullptr;
    if (f) {
        ok = fwrite(text.data(), 1, text.size(), f) == text.size();
        ok = fclose(f) == 0 && ok;
    }
    std::error_code error;
    if (ok) std::filesystem::rename(temp, path, error);
    if (!ok || error) DbgLogPublic("ServerTrust: no se pudo guardar la rotación de la clave del server");
}

} // namespace

void SetServer(const std::string& cacheDir) { s_Dir = cacheDir; }

Result Check(const std::uint8_t publicKey[KeySize],
             const std::uint8_t* previousKey,
             const std::uint8_t* rotationSignature)
{
    const char* const compiled = ServerConfig::ServerPublicKey;
    if (*compiled) {
        std::uint8_t root[KeySize];
        if (!ParseKey(compiled, root)) {
            // Mal escrita no es lo mismo que vacía: no se vuelve al primer contacto.
            DbgLogPublic("ServerTrust: ServerPublicKey tiene que tener 64 dígitos hex, no se confía en ningún server");
            return Result::Mismatch;
        }
        const std::string path = ChainFile(root);
        std::string chain = ReadChain(path);
        const Result result = DecideFromRoot(root, chain, publicKey, previousKey, rotationSignature);
        if (result == Result::Rotated) WriteChain(path, chain);
        return result;
    }
    std::uint8_t pinned[KeySize] = {};
    const bool hasPinned = ReadPin(pinned);
    const Result result = Decide(hasPinned, pinned, publicKey, previousKey, rotationSignature);
    if (result == Result::FirstUse || result == Result::Rotated) WritePin(publicKey);
    return result;
}

} // namespace ServerTrust
