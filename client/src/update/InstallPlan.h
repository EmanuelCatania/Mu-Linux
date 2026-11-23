#pragma once

// Qué hay que bajar: resuelve las capas de todos los canales contra lo que ya
// está en disco y arma la cola de pedidos HTTP Range.
//
// Capas: gana overlay > HD > base (por prioridad).  Si una capa pisa un archivo,
// también saca de las de abajo su "-hd" (casa.ozj tapa a casa-hd.ozj de la base),
// salvo que esa misma capa, u otra de arriba, traiga el suyo.  Así una textura
// del admin no queda tapada por la HD del proyecto con el HD prendido.
//
// Pedidos: uno por tramo contiguo de un zip (mu-web no atiende varios rangos en
// un pedido).  Los archivos vecinos van juntos aunque los separe una cabecera,
// y si hace falta más de la mitad de un zip se pide entero.  Primero el
// launcher (self), después lo obligatorio, al final lo opcional.

#include "update/InstallIndex.h"
#include "update/Manifest.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Update {

// Dos tramos de un mismo zip separados por menos que esto van en un solo pedido.
constexpr std::uint64_t kRangeMergeGap = 64ull << 10;

struct ChannelSource
{
	const Manifest* manifest = nullptr;
	// Carpetas o URLs donde está el manifiesto, en orden de preferencia (la
	// primera es la principal, las otras espejos).  Los zips sin url salen de acá.
	std::vector<std::string> baseUrls;
	int priority = 0;            // mayor gana (base 0, HD 10, overlay 20)
};

struct PlanOptions
{
	std::string root;            // carpeta del cliente
	std::string platform;        // "windows-x64"...: los archivos de otra plataforma no se instalan
	bool fullVerify = false;     // rehashear todo, sin confiar en el índice
	// Con el juego abierto: sólo agregar archivos opcionales que faltan.  Nada
	// que reemplace o borre (en Windows un archivo abierto no se puede pisar).
	bool gameRunning = false;
	// false: no planear el launcher (self).  Hasta que el launcher sepa
	// reemplazarse a sí mismo, un archivo que está corriendo no se pisa.
	bool includeSelf = true;
};

struct PlanFile
{
	std::size_t channel = 0;     // índice en las fuentes
	const FileEntry* entry = nullptr;
	const DeltaRef* delta = nullptr;   // no nulo: se baja el delta y se aplica al archivo en disco
};

struct Piece
{
	std::size_t file = 0;        // índice en Plan::files
	std::uint64_t offset = 0;    // dentro del pedido
	std::uint64_t packed = 0;
};

struct RangeRequest
{
	std::size_t channel = 0;
	std::uint32_t bundle = 0;
	std::uint64_t offset = 0;    // dentro del zip
	std::uint64_t length = 0;
	bool self = false;
	bool required = false;
	std::vector<Piece> pieces;
};

struct Plan
{
	std::vector<PlanFile> files;            // lo que hay que instalar
	std::vector<RangeRequest> requests;     // en el orden en que conviene bajarlos
	std::vector<std::string> remove;        // rutas a borrar (las instaló el launcher y ya no están)
	std::uint64_t downloadBytes = 0;        // suma de los pedidos
	std::uint64_t installBytes = 0;         // tamaño final de lo que se instala
	std::size_t upToDate = 0;
	bool selfUpdate = false;                // hay launcher nuevo: va antes que el resto
	// Archivos que no se pueden leer (permisos, en uso): se intentan reemplazar
	// igual, pero sirve para avisar.
	std::vector<std::string> unreadable;
};

// "Content/a/casa.ozj" -> "Content/a/casa-hd.ozj".  Vacío si ya es "-hd" o no
// tiene extensión.
std::string HdVariant(const std::string& path);

// Arma el plan.  Actualiza el índice con lo que rehashea (y le asigna el canal a
// los archivos que ya estaban bien), sin guardarlo.  false si una fuente es
// inválida (sin manifiesto, sin URL).
bool BuildPlan(const std::vector<ChannelSource>& sources, const PlanOptions& options, InstallIndex& index,
               Plan& out, std::string* error);

// URL de un zip de una fuente: la absoluta del manifiesto, o base + "/" + nombre.
std::vector<std::string> BundleUrls(const ChannelSource& source, std::uint32_t bundle);

} // namespace Update
