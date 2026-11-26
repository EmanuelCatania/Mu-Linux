#include "platform/SDL3/SDLPlatform.h"
#include "platform/IWindow.h"
#include <array>
#include <utility>
#include <cmath>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
// Joystick táctil de la página (shell.html): palanca -1..1 y botones
// sostenidos (bit 0 atacar, bit 1 skill).
EM_JS(double, mu_touch_pad_x, (), { return Module.muPad ? Module.muPad.x : 0; });
EM_JS(double, mu_touch_pad_y, (), { return Module.muPad ? Module.muPad.y : 0; });
EM_JS(int, mu_touch_pad_buttons, (), { return Module.muPad ? Module.muPad.buttons : 0; });
// Ranura de skill elegida en la página (-1 = ninguna); se lee una vez.
EM_JS(int, mu_touch_pad_select, (), {
    const pad = Module.muPad;
    if (!pad || pad.select === undefined || pad.select < 0) return -1;
    const slot = pad.select; pad.select = -1; return slot;
});
#endif
namespace Platform {
void DeliverSDLInput(const InputEvent& event);
namespace {
std::array<bool, 256> held{};
std::array<bool, 256> pressed{};
bool composing = false;
float wheelRemainder = 0;
// Un click rápido (apretar y soltar entre dos frames del juego) llegaba entero
// antes de que el juego viera el botón apretado, y se perdía.  Pasa en el
// navegador, donde los eventos entran mientras el juego cede el control, y con
// toques en pantallas táctiles.  El soltar se retiene hasta que el juego haya
// corrido un frame con el botón apretado (dos bombeos después del apretar).
unsigned pumpSerial = 0;
struct HeldButton { unsigned downSerial = 0; bool upPending = false; InputEvent up{}; int key = 0; };
// Navegador: un toque llega como apretar y soltar casi juntos, y el juego
// necesita ver el botón apretado al menos un cuadro.  En escritorio el mouse
// se entrega tal cual llega.
#ifdef __EMSCRIPTEN__
constexpr bool DeferQuickRelease = true;
#else
constexpr bool DeferQuickRelease = false;
#endif
std::array<HeldButton, 3> buttons{};   // izquierdo, derecho, medio
int VirtualKey(SDL_Scancode key) {
    if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z) return 'A' + key - SDL_SCANCODE_A;
    if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9) return '1' + key - SDL_SCANCODE_1;
    if (key >= SDL_SCANCODE_F1 && key <= SDL_SCANCODE_F12) return 0x70 + key - SDL_SCANCODE_F1;
    if (key >= SDL_SCANCODE_KP_1 && key <= SDL_SCANCODE_KP_9) return 0x61 + key - SDL_SCANCODE_KP_1;
    switch (key) {
    case SDL_SCANCODE_0: return '0';
    case SDL_SCANCODE_BACKSPACE: return 8;
    case SDL_SCANCODE_TAB: return 9;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return 13;
    case SDL_SCANCODE_ESCAPE: return 27;
    case SDL_SCANCODE_SPACE: return 32;
    case SDL_SCANCODE_LSHIFT: return 0xa0;
    case SDL_SCANCODE_RSHIFT: return 0xa1;
    case SDL_SCANCODE_LCTRL: return 0xa2;
    case SDL_SCANCODE_RCTRL: return 0xa3;
    case SDL_SCANCODE_LALT: return 0xa4;
    case SDL_SCANCODE_RALT: return 0xa5;
    case SDL_SCANCODE_CAPSLOCK: return 20;
    case SDL_SCANCODE_PAGEUP: return 33;
    case SDL_SCANCODE_PAGEDOWN: return 34;
    case SDL_SCANCODE_END: return 35;
    case SDL_SCANCODE_HOME: return 36;
    case SDL_SCANCODE_LEFT: return 37;
    case SDL_SCANCODE_UP: return 38;
    case SDL_SCANCODE_RIGHT: return 39;
    case SDL_SCANCODE_DOWN: return 40;
    case SDL_SCANCODE_PRINTSCREEN: return 44;
    case SDL_SCANCODE_INSERT: return 45;
    case SDL_SCANCODE_DELETE: return 46;
    case SDL_SCANCODE_KP_0: return 0x60;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x6a;
    case SDL_SCANCODE_KP_PLUS: return 0x6b;
    case SDL_SCANCODE_KP_MINUS: return 0x6d;
    case SDL_SCANCODE_KP_PERIOD: return 0x6e;
    case SDL_SCANCODE_KP_DIVIDE: return 0x6f;
    default: return 0;
    }
}
void SetKey(int key, bool down) {
    if (!key) return;
    if (down && !held[key]) pressed[key] = true;
    held[key] = down;
}
// ── Mando (gamepad) ──────────────────────────────────────────────────────────
// DESVIACION: el 0.97k no tiene mando.  Reparto (nombres de Xbox):
//   palanca izquierda  caminar            palanca derecha  mover el cursor
//   A                  skill elegido      X / gatillo der. golpe básico
//   B                  Escape             Y                inventario (V)
//   cruceta izq./der.  skill anterior / siguiente
//   cruceta arriba     poción E           cruceta abajo    menú de mapas (M)
//   LB / RB            pociones Q / W
//   Start              Escape             Back             personaje (C)
//   gatillo izq.       click izquierdo    apretar palanca der.  click derecho
SDL_Gamepad* gamepad = nullptr;
PadState padState{};
std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> padHeld{};
bool padLeftClick = false, padRightClick = false;
float padCursorX = 0, padCursorY = 0;
constexpr float PadDeadZone = 0.25f;

float PadAxis(SDL_GamepadAxis axis) {
    const float value = SDL_GetGamepadAxis(gamepad, axis) / 32767.0f;
    return std::fabs(value) < PadDeadZone ? 0.0f : value;
}
void PadKey(SDL_GamepadButton button, int key, bool text = false) {
    const bool down = SDL_GetGamepadButton(gamepad, button);
    if (down == padHeld[button]) return;
    padHeld[button] = down;
    SetKey(key, down);
    if (down && text) { InputEvent event{InputEventType::Text}; event.character = uint32_t(key); DeliverSDLInput(event); }
}
// Click del mando en la posición actual del cursor.
void PadClick(bool down, bool& state, InputEventType downType, InputEventType upType, int key) {
    if (down == state) return;
    state = down;
    float x = 0, y = 0;
    SDL_GetMouseState(&x, &y);
    InputEvent event{down ? downType : upType};
    event.x = int(x); event.y = int(y);
    SetKey(key, down);
    DeliverSDLInput(event);
}
void ServiceGamepad() {
    if (gamepad && !SDL_GamepadConnected(gamepad)) {
        SDL_CloseGamepad(gamepad);
        gamepad = nullptr; padState = {}; padHeld.fill(false);
    }
    if (!gamepad) {
        // Buscar un mando cuesta poco, pero no hace falta en cada cuadro.
        if (pumpSerial % 120 != 1) return;
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        if (ids && count > 0) gamepad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
        if (!gamepad) return;
    }
    SDL_UpdateGamepads();
    padState.moveX = PadAxis(SDL_GAMEPAD_AXIS_LEFTX);
    padState.moveY = PadAxis(SDL_GAMEPAD_AXIS_LEFTY);
    padState.skill = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_SOUTH);
    padState.attack = SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_WEST)
        || SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000;
    for (const auto [button, step] : {std::pair{SDL_GAMEPAD_BUTTON_DPAD_LEFT, -1}, std::pair{SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 1}}) {
        const bool down = SDL_GetGamepadButton(gamepad, button);
        if (down && !padHeld[button]) padState.cycleSkill = step;
        padHeld[button] = down;
    }
    PadKey(SDL_GAMEPAD_BUTTON_EAST, 27, true);
    PadKey(SDL_GAMEPAD_BUTTON_START, 27, true);
    PadKey(SDL_GAMEPAD_BUTTON_NORTH, 'V');
    PadKey(SDL_GAMEPAD_BUTTON_BACK, 'C');
    PadKey(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 'Q');
    PadKey(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 'W');
    PadKey(SDL_GAMEPAD_BUTTON_DPAD_UP, 'E');
    PadKey(SDL_GAMEPAD_BUTTON_DPAD_DOWN, 'M');
    // Palanca derecha: mueve el cursor del sistema (los paneles se manejan
    // igual que con mouse).  Se acumula la fracción: a baja velocidad el paso
    // por cuadro es menor que un píxel.
    const float cursorX = PadAxis(SDL_GAMEPAD_AXIS_RIGHTX), cursorY = PadAxis(SDL_GAMEPAD_AXIS_RIGHTY);
    if (cursorX != 0 || cursorY != 0) {
        float x = 0, y = 0;
        SDL_GetMouseState(&x, &y);
        padCursorX += cursorX * 12.0f; padCursorY += cursorY * 12.0f;
        const float stepX = std::trunc(padCursorX), stepY = std::trunc(padCursorY);
        padCursorX -= stepX; padCursorY -= stepY;
        if (stepX != 0 || stepY != 0) SDL_WarpMouseInWindow(SDLWindow(), x + stepX, y + stepY);
    }
    PadClick(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000, padLeftClick,
             InputEventType::LeftDown, InputEventType::LeftUp, 1);
    PadClick(SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_STICK), padRightClick,
             InputEventType::RightDown, InputEventType::RightUp, 2);
}
void Composition(bool active) {
    if (composing == active) return;
    composing = active;
    DeliverSDLInput({active ? InputEventType::ImeStart : InputEventType::ImeEnd});
}
void Text(const char* utf8) {
    Composition(false);
    // El texto del cliente es UTF-8 (Fase 4): se entrega cada code point tal
    // cual y el campo de texto lo guarda en UTF-8. Lo inválido se descarta.
    const auto* p = reinterpret_cast<const unsigned char*>(utf8);
    while (*p) {
        uint32_t code = *p++;
        int rest = 0;
        if (code >= 0xf0) { code &= 7; rest = 3; }
        else if (code >= 0xe0) { code &= 15; rest = 2; }
        else if (code >= 0xc0) { code &= 31; rest = 1; }
        else if (code >= 0x80) continue;
        while (rest-- > 0) { if ((*p & 0xc0) != 0x80) return; code = (code << 6) | (*p++ & 63); }
        InputEvent event{InputEventType::Text}; event.character = code; DeliverSDLInput(event);
    }
}
bool SDLCALL Watch(void*, SDL_Event* event) {
    if (!SDL_IsMainThread()) return true;
    InputEvent input{};
    switch (event->type) {
    case SDL_EVENT_KEY_DOWN: case SDL_EVENT_KEY_UP: {
        const int key = VirtualKey(event->key.scancode);
        SetKey(key, event->key.down);
        SetKey(16, held[0xa0] || held[0xa1]);
        SetKey(17, held[0xa2] || held[0xa3]);
        SetKey(18, held[0xa4] || held[0xa5]);
        if (event->key.down && (key == 8 || key == 9 || key == 13 || key == 27)) {
            input.type = InputEventType::Text; input.character = key; DeliverSDLInput(input);
        }
        // Ctrl+V: SDL no genera texto para la combinación; se entrega el mismo
        // carácter de control que arma TranslateMessage en Win32.
        if (event->key.down && key == 'V' && held[17] && !held[18]) {
            input.type = InputEventType::Text; input.character = 0x16; DeliverSDLInput(input);
        }
        return true;
    }
    case SDL_EVENT_TEXT_INPUT: Text(event->text.text); return true;
    case SDL_EVENT_TEXT_EDITING: Composition(event->edit.text && *event->edit.text); return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        held.fill(false); pressed.fill(false); Composition(false);
        for (HeldButton& button : buttons) button.upPending = false;
        DeliverSDLInput({InputEventType::LeftUp}); DeliverSDLInput({InputEventType::RightUp});
        DeliverSDLInput({InputEventType::MiddleUp}); return true;
    case SDL_EVENT_MOUSE_MOTION:
        input.type = InputEventType::MouseMove; input.x = int(event->motion.x); input.y = int(event->motion.y); break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP: {
        const bool down = event->button.down;
        input.x = int(event->button.x); input.y = int(event->button.y);
        switch (event->button.button) {
        case SDL_BUTTON_LEFT:
            input.type = down ? (event->button.clicks == 2 ? InputEventType::LeftDoubleClick : InputEventType::LeftDown) : InputEventType::LeftUp; break;
        case SDL_BUTTON_RIGHT: input.type = down ? InputEventType::RightDown : InputEventType::RightUp; break;
        case SDL_BUTTON_MIDDLE: input.type = down ? InputEventType::MiddleDown : InputEventType::MiddleUp; break;
        default: return true;
        }
        const int index = event->button.button == SDL_BUTTON_LEFT ? 0 : event->button.button == SDL_BUTTON_RIGHT ? 1 : 2;
        HeldButton& button = buttons[index];
        button.key = index == 0 ? 1 : index == 1 ? 2 : 4;
        if (down) {
            if (button.upPending) { SetKey(button.key, false); DeliverSDLInput(button.up); button.upPending = false; }
            button.downSerial = pumpSerial;
            SetKey(button.key, true);
        } else if (DeferQuickRelease && pumpSerial < button.downSerial + 2) {
            button.up = input; button.upPending = true;
            return true;
        } else {
            SetKey(button.key, false);
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        wheelRemainder += event->wheel.y * (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
        input.type = InputEventType::Wheel;
        input.wheel = int(wheelRemainder) * 120; wheelRemainder -= int(wheelRemainder);
        if (!input.wheel) return true;
        break;
    case SDL_EVENT_QUIT: case SDL_EVENT_WINDOW_CLOSE_REQUESTED: GetWindow().RequestCloseApplication(); return true;
    default: return true;
    }
    DeliverSDLInput(input);
    return true;
}
class SDLInput final : public IInput {
public:
    bool HasImeModeIndicator() const override {
#ifdef _WIN32
        return true;
#else
        // DESVIACION aprobada: Linux no muestra un modo IMM que SDL no proporciona.
        return false;
#endif
    }
    int16_t KeyState(int key) override {
        if (key < 0 || key >= 256) return 0;
        const auto result = static_cast<int16_t>((held[key] ? 0x8000 : 0) | (pressed[key] ? 1 : 0));
        pressed[key] = false;
        return result;
    }
    void CaptureMouse(bool capture) override { SDL_CaptureMouse(capture); }
    ImeState ReadImeState(bool clearActive) override {
        ImeState result{composing ? 1u : 0u, 0};
        // SDL expresa composición, no los bits de modo de idioma de IMM32.
        if (clearActive && composing) { SDL_ClearComposition(SDLWindow()); Composition(false); }
        return result;
    }
    void PositionIme(int x, int y, int width, int height) override {
        SDL_Rect area{x, y, width > x ? width - x : 1, 1};
        SDL_SetTextInputArea(SDLWindow(), &area, 0);
    }
    PadState Pad() override {
        PadState result = padState;
        padState.cycleSkill = 0;
#ifdef __EMSCRIPTEN__
        result.selectSkill = mu_touch_pad_select();
        // El joystick táctil se suma al mando: gana el que esté en uso.
        const float touchX = float(mu_touch_pad_x()), touchY = float(mu_touch_pad_y());
        if (touchX != 0 || touchY != 0) { result.moveX = touchX; result.moveY = touchY; }
        const int buttons = mu_touch_pad_buttons();
        result.attack = result.attack || (buttons & 1);
        result.skill = result.skill || (buttons & 2);
#endif
        return result;
    }
    std::string ClipboardText() override {
        std::string result;
        if (char* text = SDL_GetClipboardText()) { result = text; SDL_free(text); }
        return result;
    }
};
}
IInput& GetInput() { static SDLInput input; return input; }
void InstallSDLInput() {
    held.fill(false); pressed.fill(false); composing = false; wheelRemainder = 0;
    // Sin soporte de mando en el sistema el juego sigue igual, con mouse.
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    SDL_AddEventWatch(Watch, nullptr);
}
void RemoveSDLInput() {
    SDL_RemoveEventWatch(Watch, nullptr);
    if (gamepad) { SDL_CloseGamepad(gamepad); gamepad = nullptr; }
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
void ServiceSDLEvents(bool pumpNative) {
    ++pumpSerial;
    for (HeldButton& button : buttons) {
        if (button.upPending && pumpSerial >= button.downSerial + 2) {
            button.upPending = false;
            SetKey(button.key, false);
            DeliverSDLInput(button.up);
        }
    }
    if (pumpNative) SDL_PumpEvents();
    ServiceGamepad();
    // El watch ya entregó los eventos sin diferirlos respecto del pump del host.
    SDL_FlushEvents(SDL_EVENT_FIRST, SDL_EVENT_LAST);
}
}
