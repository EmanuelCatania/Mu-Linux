#pragma once

// Accesos directos al launcher.  Windows: .lnk en el escritorio y en el menú de
// inicio.  Linux: .desktop en el menú de aplicaciones y en el escritorio, con
// el ícono del launcher.  El launcher vive en Launcher/ dentro de la carpeta
// del juego, así que sin esto el jugador tendría que armarlo a mano.

#include <string>

namespace Launcher {

// exe: la ruta del launcher (en un AppImage, la del .AppImage).  name: el
// título.  Crea los que pueda; false (con el motivo) si no pudo ninguno.
bool CreateShortcuts(const std::string& exe, const std::string& name, std::string* why);

} // namespace Launcher
