#pragma once

// Manifiesto de un canal de parches (manifest.json + manifest.json.sig).
//
// Describe el ESTADO FINAL del canal, no un cambio: un cliente que se salteó
// varios parches baja sólo la última versión de cada archivo.  Cada archivo
// apunta al tramo exacto de un zip (bundle, offset, packed), así el launcher lo
// pide con un solo HTTP Range.  Los zips de versiones anteriores siguen
// sirviendo para lo que no cambió: los archivos se identifican por hash.
//
// Canales: "mu-linux/base" y "mu-linux/hd" (los publica el proyecto; nunca
// traen ejecutables) y "<server>/overlay" (el del operador; siempre trae su
// ejecutable).

#include "update/Hash.h"
#include "update/Zip.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Update {

constexpr int kManifestFormat = 1;

struct BundleRef
{
	std::string name;        // nombre del .zip (sin carpetas)
	std::uint64_t size = 0;
	Digest hash{};           // del zip entero
	std::string url;         // vacío: junto al manifiesto
};

struct DeltaRef
{
	Digest from{};           // hash del archivo viejo al que se aplica
	std::uint32_t bundle = 0;
	std::uint64_t offset = 0;   // datos del parche zstd dentro del zip
	std::uint64_t packed = 0;
};

struct FileEntry
{
	std::string path;        // relativa a la carpeta del cliente, con "/"
	std::uint64_t size = 0;
	Digest hash{};
	std::uint32_t bundle = 0;
	std::uint64_t offset = 0;   // primer byte de los datos dentro del zip
	std::uint64_t packed = 0;
	ZipMethod method = ZipMethod::Zstd;
	bool required = true;    // false: se puede jugar mientras baja
	std::string platform;    // vacío: todas
	bool self = false;       // el launcher (se actualiza antes que todo)
	std::vector<DeltaRef> deltas;
};

struct Manifest
{
	int format = kManifestFormat;
	std::string channel;
	std::string version;
	std::uint64_t sequence = 0;     // sólo crece: un manifiesto viejo con firma válida no se acepta
	std::string requiresBase;       // versión del canal base que necesita (vacío: ninguna)
	int requiresProtocol = 0;       // 0: no exige
	std::vector<std::string> platforms;
	std::vector<BundleRef> bundles;
	std::vector<FileEntry> files;
	std::vector<std::string> deleted;   // rutas a borrar del cliente
};

std::string DumpManifest(const Manifest& m);

// Valida todo lo que el launcher va a usar: rutas seguras y sin repetir,
// índices y tramos dentro de cada zip, versiones y nombres con formato.
// *unknownFormat queda en true si el manifiesto es de un formato más nuevo (el
// launcher sólo puede bajar su propia actualización).
bool ParseManifest(const std::string& text, Manifest& out, std::string* error, bool* unknownFormat = nullptr);

bool IsValidVersion(const std::string& v);          // "0.97.50"
bool IsValidChannel(const std::string& c);          // "mu-linux/base"
bool IsValidPlatform(const std::string& p);         // "windows-x64"
// -1, 0 o 1 comparando componente por componente.
int CompareVersions(const std::string& a, const std::string& b);

const char* MethodName(ZipMethod m);

} // namespace Update
