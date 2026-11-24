#pragma once

// La web del server dentro del launcher, en el panel de la imagen (el
// Web_panel de los diseños de kayito).  Muestra la dirección que anuncia el
// server (launcher.json "web", o MU_WEB_URL) y sólo cuando terminó de cargar:
// hasta entonces, o si no carga, queda la imagen del tema con las noticias.
//
// Como en el original, se navega sólo dentro del sitio del server: un enlace a
// otro dominio o una ventana nueva se abren en el navegador del sistema.
//
// Windows: WebView2 (viene con Windows 10/11; si falta, no hay panel web).
// Otros sistemas: sin panel web (meter WebKitGTK en una ventana SDL no es
// viable); Available() da false y el launcher sigue con la imagen.

#include <SDL3/SDL.h>

#include <memory>
#include <string>

namespace Launcher {

class WebPanel
{
public:
	// userDataDir: donde WebView2 guarda su caché (User/Cache/WebView2).
	WebPanel(SDL_Window* window, const std::string& userDataDir);
	~WebPanel();
	WebPanel(const WebPanel&) = delete;
	WebPanel& operator=(const WebPanel&) = delete;

	static bool Available();

	// Empieza a cargar url (http o https; otra cosa se ignora).  Se puede llamar
	// antes de que el control esté listo: navega cuando lo esté.
	void Navigate(const std::string& url);
	// El sitio cargó bien al menos una vez.
	bool Loaded() const;
	// Dónde va, en píxeles de la ventana, y si se ve.  Sólo se muestra cuando
	// cargó: show es "el launcher quiere mostrarlo ahora".
	void Place(const SDL_Rect& pixels, bool show);

	struct Impl;

private:
	std::unique_ptr<Impl> m;
};

} // namespace Launcher
