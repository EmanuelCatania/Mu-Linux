#pragma once

// Detecciones del guardián (MU-Guard, paso 3; diseño en
// doc-port/diseno-seguridad-fase5.md, sección 3).  Corren en el guardián, que
// es el padre del juego: lo mira desde afuera, por el handle que le dio
// CreateProcess (Windows) o por /proc (Linux), sin inyectar nada.
//
// Lo que cierran el juego:
//   DEBUGGER  depurador sobre el juego o el guardián
//   MEMEDIT   el driver de Cheat Engine (Windows) u otro proceso con
//             /proc/<juego>/mem abierto (Linux)
//   INJECT    módulos conocidos de Cheat Engine (vehdebug, speedhack...) y
//             funciones de tiempo del sistema reescritas dentro del juego: el
//             speedhack de Cheat Engine no aparece como módulo, pero tiene que
//             engancharlas (Windows)
// Lo que sólo se reporta, hasta probarlo contra Cheat Engine y antivirus y
// tener las listas del server (paso 5):
//   MEMSCAN   páginas señuelo leídas (Windows)
//   INJECT    otros módulos fuera del juego y del sistema (overlays de Discord,
//             Steam, OBS... son legítimos)

#include <cstdint>
#include <string>
#include <vector>

namespace Guard {

struct Finding
{
	std::uint8_t code = 0;   // GuardProto::Detection
	bool closes = false;     // cierra el juego
	std::string detail;      // qué se vio (para el log del server y el banner)
};

class Detector
{
public:
	Detector();
	~Detector();
	Detector(const Detector&) = delete;
	Detector& operator=(const Detector&) = delete;

	// process/thread: los handles del juego que dio CreateProcess (Windows; en
	// Linux nullptr).  pid: el del juego.  gameDir: su carpeta.  Antes de
	// reanudar el juego: planta las páginas señuelo.
	void Attach(void* process, void* mainThread, unsigned long pid, const std::string& gameDir);

	// Una pasada.  Lo nuevo desde la anterior va a `found` (un módulo o una
	// página ya reportados no se repiten).
	void Scan(std::vector<Finding>& found);

private:
	struct State;
	State* m;
};

} // namespace Guard
