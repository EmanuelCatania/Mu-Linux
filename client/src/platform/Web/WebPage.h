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
// La página detectó pantalla táctil (argumento --mobile).
bool IsTouch();
#else
inline bool IsTouch() { return false; }
#endif
}
