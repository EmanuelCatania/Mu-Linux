#include "stdafx.h"
#include "platform/Web/WebPage.h"
#ifdef __EMSCRIPTEN__
#include "app/core/Window.h"
#include "app/config/UserSettings.h"
#include "platform/IWindow.h"
#include "platform/ITimer.h"
#include "game/core/FrameLimiter.h"
#include <emscripten.h>

// Con la pestaña oculta el documento puede medir 0: se usa la ventana.
EM_JS(int, mu_page_width, (), { return (document.documentElement.clientWidth || innerWidth) | 0; });
EM_JS(int, mu_page_height, (), { return (document.documentElement.clientHeight || innerHeight) | 0; });
// Píxeles de pantalla por píxel CSS: 2 o 3 en un celular, más de 1 en un
// monitor con escala.
EM_JS(double, mu_page_pixel_ratio, (), { return window.devicePixelRatio || 1; });
// Con la pestaña oculta el navegador no da cuadros: el tope de 100 ms deja
// correr la red y la lógica (si no, el server corta por inactividad).
// minMs: con un límite de FPS más bajo que el refresco del monitor se dejan
// pasar cuadros del navegador hasta cumplirlo (2 ms de margen: los cuadros
// no llegan exactos).
EM_ASYNC_JS(void, mu_wait_frame, (double minMs), {
    const wait = () => new Promise((resolve) => {
        let frame = 0, timer = 0;
        frame = requestAnimationFrame(() => { clearTimeout(timer); resolve(); });
        timer = setTimeout(() => { cancelAnimationFrame(frame); resolve(); }, 100);
    });
    // El próximo cuadro se agenda desde el anterior, no desde ahora: así el
    // promedio da el límite aunque no sea divisor del refresco (60 en 144 Hz).
    do { await wait(); } while (minMs > 0 && !document.hidden && performance.now() < (Module.muNextFrame || 0) - 2);
    const now = performance.now(), next = (Module.muNextFrame || 0) + minMs;
    Module.muNextFrame = next < now ? now + minMs : next;
});
EM_JS(int, mu_page_touch, (), {
    return (Module.arguments || []).indexOf('--mobile') >= 0 ? 1 : 0;
});
EM_JS(int, mu_page_fullscreen, (), { return document.fullscreenElement ? 1 : 0; });
// muSetFullscreen es de la página (shell.html): la misma que usa su botón.
EM_JS(void, mu_page_set_fullscreen, (int on), { if (window.muSetFullscreen) window.muSetFullscreen(!!on); });

namespace WebPage {
namespace {
// Por debajo de esto la interfaz 640x480 se achica demasiado.
constexpr DWORD MinWidth = 480, MinHeight = 360;
DWORD s_pendingWidth = 0, s_pendingHeight = 0, s_pendingSince = 0, s_lastCheck = 0;
}

void Size(DWORD& width, DWORD& height) {
    // En píxeles de pantalla, no CSS: en un celular la página mide ~800x360
    // CSS y dibujar a eso se veía pixelado al estirarlo x3.
    double ratio = mu_page_pixel_ratio();
    if (ratio < 1.0) ratio = 1.0;
    const int w = (int)(mu_page_width() * ratio), h = (int)(mu_page_height() * ratio);
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
    // La pantalla completa la maneja la página (su botón, Escape, F11): el
    // juego sólo refleja el estado, que es lo que muestra Opciones.
    const bool windowed = mu_page_fullscreen() == 0;
    if (gWindow.IsWindowMode() != windowed) gWindow.SetWindowMode(windowed, false);
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

void WaitFrame() {
    // Con el dibujo ligado al paso de 40 ms ya espera el limitador.
    const int limit = gFrameLimiter.IsRenderDecoupled() ? gFrameLimiter.FpsLimit() : 0;
    mu_wait_frame(limit > 0 ? 1000.0 / limit : 0.0);
}

bool IsTouch() { return mu_page_touch() != 0; }

bool IsFullscreen() { return mu_page_fullscreen() != 0; }

void SetFullscreen(bool on) { mu_page_set_fullscreen(on ? 1 : 0); }
}
#endif
