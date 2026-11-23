#pragma once

// Ejecuta un plan: baja los pedidos en paralelo, verifica cada archivo y lo
// reemplaza de forma atómica; recién ahí actualiza el índice.
//
// - Lo que baja va a downloadDir (User/Cache/downloads, mismo volumen que el
//   cliente): el cliente lista carpetas de Content y vería archivos a medias.
// - Un pedido cortado se retoma donde quedó (el .part lleva en el nombre el hash
//   del zip y el tramo, así que nunca se mezcla con otro).
// - Reintentos con espera exponencial; cada reintento prueba la URL siguiente
//   (los espejos de la fuente).
// - Un delta que no da el archivo esperado se reemplaza por el archivo entero.
// - Si un archivo no se puede reemplazar (en Windows, abierto por el juego) se
//   anota y se sigue con el resto.
// - El launcher (self) no se pisa: el que está se renombra a "<ruta>.old" y el
//   nuevo va a su lugar (ver src/launcher/SelfUpdate.h).

#include "update/InstallIndex.h"
#include "update/InstallPlan.h"
#include "update/Transfer.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Update {

struct InstallStatus
{
	std::uint64_t downloaded = 0;
	std::uint64_t downloadTotal = 0;
	std::size_t filesDone = 0;
	std::size_t filesTotal = 0;
	std::string lastFile;
};

struct InstallOptions
{
	std::string root;              // carpeta del cliente
	std::string downloadDir;       // temporales (User/Cache/downloads)
	std::string indexPath;         // User/Cache/install.json ("" = no se guarda)
	Transport* transport = nullptr;
	RateLimiter* limiter = nullptr;   // nullptr: sin límite
	int connections = 4;
	int maxAttempts = 5;           // por pedido
	unsigned backoffMs = 500;      // primera espera; se duplica hasta backoffMaxMs
	unsigned backoffMaxMs = 30000;
	bool selfOnly = false;         // sólo el launcher (va antes que todo, ver 3.1)
	const std::atomic<bool>* cancel = nullptr;
	// Se llama desde los hilos de descarga, de a uno por vez, como mucho cada
	// ~100 ms y al terminar cada archivo.
	std::function<void(const InstallStatus&)> progress;
	std::function<void(const std::string&)> log;
};

struct InstallResult
{
	std::size_t installed = 0;
	std::size_t removed = 0;
	std::size_t deltasApplied = 0;
	std::size_t deltaFallbacks = 0;   // deltas que no sirvieron: se bajó el archivo entero
	std::uint64_t downloaded = 0;
	bool cancelled = false;
	std::vector<std::string> failures;   // "ruta: motivo"
};

// true si quedó todo instalado (y, sin selfOnly, borrado lo que sobraba).
bool RunPlan(const std::vector<ChannelSource>& sources, const Plan& plan, InstallIndex& index,
             const InstallOptions& options, InstallResult& out);

} // namespace Update
