#include "stdafx.h"
#include "platform/Web/WebPage.h"
#ifdef __EMSCRIPTEN__
#include "app/core/Window.h"
#include "app/config/UserSettings.h"
#include "platform/IWindow.h"
#include "platform/ITimer.h"
#include <emscripten.h>

// Con la pestaña oculta el documento puede medir 0: se usa la ventana.
EM_JS(int, mu_page_width, (), { return (document.documentElement.clientWidth || innerWidth) | 0; });
EM_JS(int, mu_page_height, (), { return (document.documentElement.clientHeight || innerHeight) | 0; });
// Con la pestaña oculta el navegador no da cuadros: el tope de 100 ms deja
// correr la red y la lógica (si no, el server corta por inactividad).
EM_ASYNC_JS(void, mu_wait_frame, (), {
    await new Promise((resolve) => { requestAnimationFrame(resolve); setTimeout(resolve, 100); });
});
EM_JS(int, mu_page_touch, (), {
    return (Module.arguments || []).indexOf('--mobile') >= 0 ? 1 : 0;
});

namespace WebPage {
namespace {
// Por debajo de esto la interfaz 640x480 se achica demasiado.
constexpr DWORD MinWidth = 480, MinHeight = 360;
DWORD s_pendingWidth = 0, s_pendingHeight = 0, s_pendingSince = 0, s_lastCheck = 0;
}

void Size(DWORD& width, DWORD& height) {
    const int w = mu_page_width(), h = mu_page_height();
    width = w > (int)MinWidth ? (DWORD)w : MinWidth;
    height = h > (int)MinHeight ? (DWORD)h : MinHeight;
    // Con una resolución elegida en Opciones se dibuja con ese alto (si es más
    // chico que la página) y el ancho de la proporción de la página: el
    // navegador lo estira sin deformar.  Más bajo = más cuadros por segundo.
    DWORD chosenWidth = 0, chosenHeight = 0;
    if (CUserSettings::GetResolutionSize(gUserSettings.GetResolution(), &chosenWidth, &chosenHeight)
        && chosenHeight >= MinHeight && chosenHeight < height) {
        width = (DWORD)((unsigned long long)width * chosenHeight / height);
        height = chosenHeight;
    }
}

void Tick() {
    if (!Platform::GetWindow().IsCreated()) return;
    const DWORD now = Platform::GetTimer().Ticks();
    if (now - s_lastCheck < 100) return;
    s_lastCheck = now;
    DWORD width, height;
    Size(width, height);
    if (width == gWindow.GetRealWidth() && height == gWindow.GetHeight()) { s_pendingWidth = 0; return; }
    // Mientras el jugador arrastra el borde el tamaño cambia en cada evento:
    // se aplica cuando queda quieto 250 ms.
    if (width != s_pendingWidth || height != s_pendingHeight) {
        s_pendingWidth = width; s_pendingHeight = height; s_pendingSince = now;
        return;
    }
    if (now - s_pendingSince < 250) return;
    s_pendingWidth = 0;
    gWindow.ChangeMode(true, false, width, height);
}

void WaitFrame() { mu_wait_frame(); }

bool IsTouch() { return mu_page_touch() != 0; }
}
#endif
