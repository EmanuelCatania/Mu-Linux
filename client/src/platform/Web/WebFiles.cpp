#include "platform/Web/WebFiles.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/threading.h>

// Module.muPending (lo arma la página): ruta absoluta en el FS -> URL.  Lo que
// ya se bajó sale del mapa.  Un archivo que falla queda vacío y se reintenta la
// próxima vez que se abra.
// El cuerpo no lleva barras invertidas ni expresiones regulares: el
// preprocesador lo pasa como texto y las rompe.
EM_ASYNC_JS(int, mu_web_fetch, (const char* rawPath), {
    const pending = Module.muPending;
    if (!pending || pending.size === 0) return 1;
    let path = UTF8ToString(rawPath).split(String.fromCharCode(92)).join('/');
    if (path.charAt(0) !== '/') path = FS.cwd() + '/' + path;
    const parts = [];
    for (const part of path.split('/')) {
        if (!part || part === '.') continue;
        if (part === '..') parts.pop(); else parts.push(part);
    }
    path = '/' + parts.join('/');
    const url = pending.get(path);
    if (!url) return 1;
    try {
        const response = await fetch(url);
        if (!response.ok) throw new Error('HTTP ' + response.status);
        FS.writeFile(path, new Uint8Array(await response.arrayBuffer()));
        pending.delete(path);
        if (Module.muOnFetched) Module.muOnFetched(path, pending.size);
        return 1;
    } catch (e) {
        console.warn('No se pudo bajar ' + url + ': ' + e.message);
        return 0;
    }
});

namespace Platform::WebFiles {
bool Ensure(const std::string& path) {
    if (path.empty() || !emscripten_is_main_runtime_thread()) return true;
    return mu_web_fetch(path.c_str()) != 0;
}
}
#endif
