#pragma once
// Se incluye después de stdafx.h (DWORD).

// La página que contiene al cliente web.  En el navegador no hay resoluciones
// fijas: la ventana del juego es el espacio que deja la página, y cambia cuando
// el jugador redimensiona el navegador o entra a pantalla completa.
namespace WebPage {
#ifdef __EMSCRIPTEN__
// Tamaño disponible en la página, en píxeles CSS (con un mínimo jugable).
void Size(DWORD& width, DWORD& height);
// Una vez por frame: si la página cambió de tamaño (y se quedó quieta un
// momento), la ventana del juego pasa a ese tamaño.
void Tick();
// Le devuelve el control al navegador hasta su próximo cuadro (refresco del
// monitor), así presenta lo dibujado y atiende eventos y red.
void WaitFrame();
// La página detectó pantalla táctil (argumento --mobile).
bool IsTouch();
// Pantalla completa de la página.  Pedirla necesita un click reciente del
// jugador: se llama desde el click en Opciones, que lo es.
bool IsFullscreen();
void SetFullscreen(bool on);
// El juego terminó (Salir del juego): la página guarda lo pendiente y vuelve
// al inicio del sitio.
void Exited();
#else
inline bool IsTouch() { return false; }
#endif
}
