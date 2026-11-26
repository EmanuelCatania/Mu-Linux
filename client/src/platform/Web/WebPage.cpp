#include "stdafx.h"
#include "platform/Web/WebPage.h"
#ifdef __EMSCRIPTEN__
#include "app/core/Window.h"
#include "app/config/UserSettings.h"
#include "platform/IWindow.h"
#include "platform/ITimer.h"
#include "game/core/FrameLimiter.h"
#include "game/core/HeroVitals.h"
#include "ui/MoveList.h"
#include "ui/PanelAnchor.h"
#include "globals.h"
#include "functions.h"
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
// Estado del héroe para la interfaz táctil de la página (#hud): la página lo
// lee de Module.muHud y se entera por muHudChanged.
EM_JS(void, mu_page_hud, (int inGame, int chat, double hp, double maxHp, double mp, double maxMp,
                          double ag, double maxAg, int level, double exp, int x, int y, int current,
                          const unsigned char* skills, const char* name, int q, int w, int e, int panel), {
    const hud = Module.muHud || (Module.muHud = {});
    hud.inGame = !!inGame; hud.chat = !!chat;
    hud.hp = hp; hud.maxHp = maxHp; hud.mp = mp; hud.maxMp = maxMp; hud.ag = ag; hud.maxAg = maxAg;
    hud.level = level; hud.exp = exp; hud.x = x; hud.y = y; hud.current = current;
    hud.skills = skills ? Array.from(HEAPU8.subarray(skills, skills + 20)) : [];
    hud.name = name ? UTF8ToString(name) : "";
    hud.potions = [q, w, e]; hud.panel = !!panel;
    if (Module.muHudChanged) Module.muHudChanged(hud);
});
// muSetFullscreen es de la página (shell.html): la misma que usa su botón.
EM_JS(void, mu_page_set_fullscreen, (int on), { if (window.muSetFullscreen) window.muSetFullscreen(!!on); });

extern "C" int __cdecl sub_482E40(int a1);
extern "C" int __cdecl GetScreenWidth(void);   // cantidad del ítem rápido Q / W / E

// Contraseña cargada en el menú de la página (interfaz táctil); 0 si no hay.
EM_JS(int, mu_page_password, (char* out, int size), {
    const pass = Module.muPassword || "";
    if (!pass) return 0;
    let n = 0;
    for (; n < pass.length && n < size - 1; n++) HEAPU8[out + n] = pass.charCodeAt(n) & 255;
    HEAPU8[out + n] = 0;
    return 1;
});

namespace WebPage {
namespace {
// Por debajo de esto la interfaz 640x480 se achica demasiado.
constexpr DWORD MinWidth = 480, MinHeight = 360;
// En celular no se dibuja a más alto que esto: a 3 píxeles de pantalla por
// píxel CSS son 2400x1080 o más, y el teléfono levantaba temperatura.
constexpr int TouchMaxHeight = 720;
DWORD s_pendingWidth = 0, s_pendingHeight = 0, s_pendingSince = 0, s_lastCheck = 0;

// Diez veces por segundo: lo que la página dibuja en su barra.
void PushHud() {
    const BYTE* attribute = (const BYTE*)CharacterAttribute;
    const bool inGame = SceneFlag == 5 && Hero && attribute;
    // Fuera del juego sólo se avisa si está el cuadro de usuario y contraseña
    // (en el campo "chat"): la página ofrece ahí la cuenta guardada.
    if (!inGame) {
        // Interfaz táctil: al aparecer el cuadro de usuario y contraseña se
        // pone la contraseña del menú de la página, para no abrir el teclado.
        // El usuario ya lo recuerda el juego.
        static bool dialog = false;
        const bool now = SceneFlag == 2 && LoginSubState == 2;
        if (now && !dialog && InputLength[1] == 0) {
            char pass[11] = {};
            if (mu_page_password(pass, sizeof(pass))) {
                memcpy(InputText[1], pass, sizeof(pass));
                InputLength[1] = (int)strlen(pass);
            }
        }
        dialog = now;
        mu_page_hud(0, now ? 1 : 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, nullptr, nullptr, 0, 0, 0, 0);
        return;
    }
    const CHARACTER* hero = AsCharacter(Hero);
    // Misma cuenta que RenderExperience (0x4BF990): lo ganado dentro del nivel.
    const int level = *(const unsigned short*)(attribute + 0xE);
    double prior = 0;
    if (level > 1) {
        const double l = level - 1;
        prior = 10.0 * l * l * (l + 9);
        if (l > 255) prior += 1000.0 * (level - 256) * (level - 256) * (level - 256 + 9);
    }
    const double need = *(const int*)(attribute + 52) - prior, have = *(const int*)(attribute + 16) - prior;
    double exp = need > 0 ? have / need : 0;
    exp = exp < 0 ? 0 : exp > 1 ? 1 : exp;
    char name[12] = {};
    memcpy(name, hero->ID, sizeof(hero->ID) < 11 ? sizeof(hero->ID) : 11);
    mu_page_hud(1, (InputEnable || TabInputEnable) ? 1 : 0,
                (double)gHeroVitals.GetCurrent(VITAL_LIFE), (double)gHeroVitals.GetMax(VITAL_LIFE),
                (double)gHeroVitals.GetCurrent(VITAL_MANA), (double)gHeroVitals.GetMax(VITAL_MANA),
                (double)gHeroVitals.GetCurrent(VITAL_AG), (double)gHeroVitals.GetMax(VITAL_AG),
                level, exp, hero->PositionX, hero->PositionY, hero->CurrentSkill,
                attribute + 87, name, sub_482E40(0), sub_482E40(1), sub_482E40(2),
                // Un panel abierto (inventario, personaje, mapas...) ocupa el
                // lugar del anillo: la página lo esconde.
                (GetScreenWidth() != gWindow.PanelOrigin(Ui::WindowRight) || gMoveList.IsOpen()) ? 1 : 0);
}
}

void Size(DWORD& width, DWORD& height) {
    // En píxeles de pantalla, no CSS: en un celular la página mide ~800x360
    // CSS y dibujar a eso se veía pixelado al estirarlo x3.
    double ratio = mu_page_pixel_ratio();
    if (ratio < 1.0) ratio = 1.0;
    if (IsTouch() && mu_page_height() * ratio > TouchMaxHeight) {
        ratio = (double)TouchMaxHeight / mu_page_height();
        if (ratio < 1.0) ratio = 1.0;
    }
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
    if (IsTouch()) { gWindow.SetTouchHud(true); PushHud(); }
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
    // Con el dibujo ligado al paso de 40 ms son 25 cuadros por segundo.  En
    // celular, sin límite elegido, 60: una pantalla de 120 Hz al doble de
    // cuadros es puro calor.
    int limit = gFrameLimiter.IsRenderDecoupled() ? gFrameLimiter.FpsLimit() : 25;
    if (limit <= 0 && IsTouch()) limit = 60;
    mu_wait_frame(limit > 0 ? 1000.0 / limit : 0.0);
}

bool IsTouch() { static const bool touch = mu_page_touch() != 0; return touch; }

bool IsFullscreen() { return mu_page_fullscreen() != 0; }

void SetFullscreen(bool on) { mu_page_set_fullscreen(on ? 1 : 0); }
}
#endif
