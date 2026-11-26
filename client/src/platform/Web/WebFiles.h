#pragma once
#include <string>

// Archivos del juego en el navegador.  La página crea Content/ y System/ como
// archivos vacíos (así las búsquedas sin mayúsculas y los listados funcionan
// sin bajar nada) y anota de qué URL sale cada uno.  Antes de leer un archivo
// hay que llamar a Ensure: si todavía no se bajó, lo baja (el juego espera, con
// ASYNCIFY) y lo escribe en su lugar.  Fuera del navegador no hace nada.
//
// Desde un hilo, le pide la bajada al hilo principal y espera el contenido.
namespace Platform::WebFiles {
#ifdef __EMSCRIPTEN__
bool Ensure(const std::string& path);
#else
inline bool Ensure(const std::string&) { return true; }
#endif
}
