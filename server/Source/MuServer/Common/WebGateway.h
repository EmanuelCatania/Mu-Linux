#pragma once

// Entrada de los clientes web (versión del cliente para el navegador).
//
// El navegador no abre TCP: habla WebSocket con mu-web, y mu-web abre una
// conexión TCP común contra este server, a un puerto propio (el del server más
// MU_WEB_PORT_OFFSET, por defecto 100) que no se publica afuera.  Lo primero
// que manda mu-web en esa conexión es una línea
//
//     MUWEB <clave> <ip del jugador>\n
//
// con la clave compartida MU_WEB_GATEWAY_KEY.  Con eso el server conoce la IP
// real del jugador (límites por IP, bans, logs) y sabe que la sesión es web
// (MU-Guard no puede correr en el navegador).  Una conexión al puerto web sin
// esa línea, o con otra clave, se cierra.
//
// Sin MU_WEB_GATEWAY_KEY (o con menos de 16 caracteres) no se abre el puerto.
// Por ahora sólo en Linux; en Windows Enabled() devuelve false.

namespace WebGateway
{
	bool Enabled();
	WORD Port(WORD serverPort);
	// Socket de escucha no bloqueante, o INVALID_SOCKET (se loguea la causa).
	SOCKET Listen(WORD port);
	// Acepta una conexión pendiente del puerto web y lee su encabezado.  Devuelve
	// el socket ya no bloqueante y la IP del jugador, o INVALID_SOCKET si no
	// había ninguna o el encabezado no vale.
	SOCKET Accept(SOCKET listenSocket, char* ip, int ipSize);
}
