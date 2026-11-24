#pragma once

// Banner de MU-Guard (Fase 5, paso 2 del diseño).  Especificación visual: el
// template seguridad/index.html (720x458, fondo, logo, título "MU-Guard", una
// línea de fase y un pie con el paso actual).  Corre en el guardián antes de
// lanzar el juego, con SDL3 y el dibujo del launcher (launcher/Gfx).
//
// Muestra sólo los pasos que el guardián hace de verdad; los de memoria,
// depuradores y herramientas llegan con sus detecciones.
//
// Sin video, o compilado sin SDL3, el banner no se abre y Wait() sólo espera:
// el guardián funciona igual.

#include <functional>

namespace Guard {

enum class BannerStep
{
	Network,   // "Validando conexión con el servidor..."
	Launch,    // "Abriendo main.exe"
};

class Banner
{
public:
	Banner();
	~Banner();
	Banner(const Banner&) = delete;
	Banner& operator=(const Banner&) = delete;

	// language: 0 castellano, 1 inglés, 2 portugués (Config.ini [Language]).
	// false si no se pudo abrir (sin video, sin las fuentes): seguir sin banner.
	bool Open(int language);

	bool IsOpen() const;

	void Step(BannerStep step);

	// Dibuja y atiende la ventana hasta que done() dé true y el paso actual lleve
	// en pantalla al menos minMs.  false si el jugador cerró el banner.
	bool Wait(const std::function<bool()>& done, int minMs);

	void Close();

private:
	struct State;
	State* m;
};

} // namespace Guard
