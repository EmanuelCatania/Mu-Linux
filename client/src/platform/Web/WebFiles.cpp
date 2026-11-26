#include "platform/Web/WebFiles.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>
#include <emscripten/em_asm.h>
#include <sys/stat.h>
#include <chrono>
#include <thread>

// Module.muPending (lo arma la página): ruta absoluta en el FS -> URL.  Lo que
// ya se bajó sale del mapa; Module.muFetch lo baja.  Un archivo que falla queda vacío y se reintenta la
// próxima vez que se abra.
// El cuerpo no lleva barras invertidas ni expresiones regulares: el
// preprocesador lo pasa como texto y las rompe.
EM_ASYNC_JS(int, mu_web_fetch, (const char* rawPath), {
    const pending = Module.muPending;
    if (!pending) return 1;
    let path = UTF8ToString(rawPath).split(String.fromCharCode(92)).join('/');
    if (path.charAt(0) !== '/') path = FS.cwd() + '/' + path;
    const parts = [];
    for (const part of path.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') parts.pop(); else parts.push(part);
    }
    path = '/' + parts.join('/');
    if (!pending.has(path)) { if (Module.muTouch) Module.muTouch(path); return 1; }
    // Module.muFetch (shell.html) no repite una bajada que ya está en curso
    // (la descarga de fondo del menú puede haberla empezado).
    return (await Module.muFetch(path, true)) ? 1 : 0;
});

namespace Platform::WebFiles {
bool Ensure(const std::string& path) {
    if (path.empty()) return true;
    if (emscripten_is_main_runtime_thread()) return mu_web_fetch(path.c_str()) != 0;
    // Un hilo (las texturas HD se leen en hilos) no puede esperar una promesa:
    // le pide la bajada al hilo principal, que la atiende entre cuadros, y
    // espera a que el archivo tenga contenido.  Un archivo perezoso nace vacío
    // y sólo se escribe completo.
    const int started = MAIN_THREAD_EM_ASM_INT({
        return Module.muFetchStart ? Module.muFetchStart(UTF8ToString($0)) : 0;
    }, path.c_str());
    if (!started) return true;
    for (int i = 0; i < 600; ++i) {   // hasta 30 s
        std::this_thread::sleep_for(std::chrono::milliseconds(i < 10 ? 5 : 50));
        // 0 bajando, 1 listo, 2 falló (no tiene sentido seguir esperando).
        const int state = MAIN_THREAD_EM_ASM_INT({
            return Module.muFetchState ? Module.muFetchState(UTF8ToString($0)) : 1;
        }, path.c_str());
        if (state == 1) return true;
        if (state == 2) return false;
    }
    return false;
}
}
#endif
