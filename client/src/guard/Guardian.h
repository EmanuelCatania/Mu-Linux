#pragma once

// MU-Guard, lado cliente (Fase 5).  Diseño en doc-port/diseno-seguridad-fase5.md;
// mensajes en guard/GuardProtocol.h.
//
// Un solo main.exe con dos roles:
//   - GUARDIÁN: el proceso que abrió quien sea (launcher, acceso directo).
//     Le pregunta al ConnectServer si hay MU-Guard (F4:16/17); si lo hay, saluda
//     a mu-guard y se relanza a sí mismo como el juego, pasándole un pipe.  Queda
//     vivo, sin ventana, sirviéndole los tickets, y sale con el código del juego.
//   - JUEGO: el hijo.  Recibe el pipe por herencia (el número viaja en una
//     variable de entorno que se borra al leerla); en cada login le pide un
//     ticket al guardián.  Si el guardián muere, el juego se cierra.
// Sin MU-Guard en el server (o si no se puede saber) el proceso sigue como el
// juego, igual que antes.
//
// Esta primera versión no tiene banner ni protege la memoria (pasos 2 y 3 del
// diseño): sólo cierra el camino de abrir el juego sin pasar por el guardián.

#include <cstdint>

namespace Guardian {

// true en el juego que lanzó un guardián.
bool IsGameRole();

// Rol GUARDIÁN: antes de crear la ventana, con la dirección del ConnectServer.
// Devuelve -1 si este proceso sigue como el juego; si no, el código de salida
// del juego, que el guardián lanzó y esperó.  argv: el de main() (en Windows se
// usa la línea de comandos del proceso).
int Run(const char* host, unsigned short port, char** argv);

// Rol JUEGO: lee el pipe heredado y vigila al guardián.  Llamarlo una vez, al
// arrancar.
void StartGameRole();

// Rol JUEGO: pide un ticket y calcula su MAC con el hash del saludo de la
// conexión actual con el GameServer (SecureChannel::TranscriptHash).  false sin
// guardián o si no hubo ticket: el login sale sin él y el GameServer decide.
bool FetchTicket(const std::uint8_t transcript[32], std::uint8_t id[16], std::uint8_t mac[32]);

} // namespace Guardian
