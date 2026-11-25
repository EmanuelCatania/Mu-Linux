#pragma once
#include "platform/Posix/PosixIdentity.h"
#include "platform/UserPath.h"
#include <cstdio>
#include <unistd.h>
namespace Platform {
// Navegador: no hay machine-id.  Se genera uno al azar la primera vez y queda
// en User/machine-id, que la página guarda en IndexedDB.  Identifica a este
// navegador, no a la máquina: borrar los datos del sitio da uno nuevo.
class WebIdentity final : public IIdentity {
public:
    bool Read(char (&output)[36], const char*& error) override {
        char path[64];
        UserPath(path, "machine-id");
        PosixIdentity file(path, path);
        if (file.Read(output, error)) return true;
        unsigned char bytes[16];
        if (getentropy(bytes, sizeof bytes) != 0) { error = "Sin entropía para la identidad del navegador"; return false; }
        char hex[34];
        for (unsigned i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", bytes[i]);
        hex[32] = '\n'; hex[33] = 0;
        if (FILE* f = std::fopen(path, "wb")) { std::fwrite(hex, 1, 33, f); std::fclose(f); }
        return file.Read(output, error);
    }
};
}
