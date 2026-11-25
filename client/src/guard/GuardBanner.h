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
#include <string>
#include <vector>

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

	// Estado AMENAZA del template: vira a rojo, muestra qué se detectó y cuenta
	// 5 s antes de volver.  Abre la ventana si estaba cerrada (language: el de
	// Open).  Sin banner posible, un cartel del sistema con el mismo texto.
	// notice: el aviso del server ("Aviso 2 de 3."), vacío si no hay.
	void Threat(int language, const std::string& what, const std::string& notice = std::string());

	// La máquina o la IP están baneadas: el juego no se abre.  Mismo estado rojo,
	// con el motivo y hasta cuándo en `detail`.
	void Blocked(int language, const std::string& detail);

	// Ya hay en esta PC todos los juegos que permite el server: no se abre otro.
	void Refused(int language, const std::string& detail);

	void Close();

private:
	// keepLast: la última línea sigue a la vista durante la cuenta regresiva.
	void Alert(int language, const char* fase, const char* headline, const std::vector<std::string>& lines, bool keepLast);

	struct State;
	State* m;
};

} // namespace Guard
