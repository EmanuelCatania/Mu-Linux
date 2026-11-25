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
//   MEMSCAN   páginas señuelo leídas: un editor escaneando la memoria del
//             juego (Windows).  Lo decide el server (Guard.toml MemScanCloses,
//             encendido por defecto): si un antivirus da falsos positivos, se
//             apaga y sólo se registra.
//   TOOL      una herramienta de las listas del server abierta (procesos o
//             ventanas, Data/Guard/Lists.toml); lo decide el server
//             (Guard.toml ToolCloses, encendido por defecto)
//   INJECT    un módulo de la lista de bloqueados del server
//   FILES     un archivo del juego (Content/) distinto del que tiene el server
//             en Data/Guard/Files.txt, o que falta; lo decide el server
//             (Guard.toml FilesCloses, encendido por defecto).  Se verifica de
//             a poco (unos MB por pasada), y otra vez cada 10 minutos
// Lo que sólo se reporta:
//   INJECT    otros módulos fuera del juego y del sistema (overlays de Discord,
//             Steam, OBS... son legítimos), salvo los de la lista de permitidos

#include "guard/GuardProtocol.h"

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

	// La DACL de los hilos del juego (Windows): se la pone a los que el juego
	// crea después de arrancar.  Tiene que vivir mientras se use el detector.
	void SetThreadDacl(void* dacl);

	// Contadores de vida (KILLED): cuántas veces corrió cada detector.
	void Life(std::uint32_t out[GuardProto::LifeCounters]) const;

	// Si MEMSCAN cierra el juego (lo dice el server en el saludo).
	void SetMemScanCloses(bool closes);

	// Si TOOL cierra el juego (también del saludo).
	void SetToolCloses(bool closes);

	// Si FILES cierra el juego (también del saludo).
	void SetFilesCloses(bool closes);

	// Una entrada de las listas del server (GuardProto::RuleKind; RuleReset
	// las borra).
	void Rule(std::uint8_t kind, const std::string& text);

	// Nombre de una detección para el banner según Messages.toml del server;
	// vacío si el server no lo cambió.  language: 0 castellano, 1 inglés, 2
	// portugués.
	std::string Name(std::uint8_t code, int language) const;

	// Una pasada.  Lo nuevo desde la anterior va a `found` (un módulo o una
	// página ya reportados no se repiten).
	void Scan(std::vector<Finding>& found);

	// Desafío de memoria (paso 4): dónde quedó cargado el ejecutable del juego
	// y la lectura de su memoria.  false si todavía no se puede.
	bool ImageBase(std::uint64_t& base);

	bool Read(std::uint64_t address, void* data, std::size_t size);

private:
	struct State;
	State* m;
};

} // namespace Guard
