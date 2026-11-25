#pragma once

// Lo mínimo que piden los .cpp compartidos de Common (ServerIdentity) para
// compilar fuera de un server: BYTE y LogAdd.  mu-guard no usa el resto del
// stdafx.h de los servers.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

typedef unsigned char BYTE;

enum eGuardLogColor
{
	LOG_BLACK = 0,
	LOG_RED = 1,
	LOG_GREEN = 2,
	LOG_BLUE = 3,
};

// Una línea con fecha en la salida estándar (los logs del contenedor).
void LogAdd(int color, const char* text, ...);
