#pragma once

// Zips de los parches.  Se usa zip y no un formato propio para que un admin los
// arme y los abra con cualquier herramienta, y porque cada archivo va
// comprimido por separado: el launcher baja con HTTP Range sólo el tramo de un
// archivo, sin el zip entero.
//
// Métodos: 0 (guardado) y 93 (zstd).  Deflate (8) se lista pero todavía no se
// descomprime: hace falta recién cuando el server acepte zips armados por el
// admin (paso 5 del diseño), y ahí los rearma en zstd.  Zip64 cuando hace falta.
// Los zips cifrados se rechazan.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Update {

enum class ZipMethod : std::uint16_t { Store = 0, Deflate = 8, Zstd = 93 };

std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t crc = 0);

struct ZipEntry
{
	std::string name;              // UTF-8, con "/"
	ZipMethod method = ZipMethod::Store;
	std::uint32_t crc = 0;
	std::uint64_t packedSize = 0;
	std::uint64_t size = 0;
	std::uint64_t headerOffset = 0;   // cabecera local
	std::uint64_t dataOffset = 0;     // primer byte de los datos (lo que pide el launcher)
};

class ZipWriter
{
public:
	ZipWriter() = default;
	~ZipWriter();
	ZipWriter(const ZipWriter&) = delete;
	ZipWriter& operator=(const ZipWriter&) = delete;

	bool Open(const std::string& path);
	// packed ya viene comprimido con method; crc y size son los del original.
	bool Add(const std::string& name, ZipMethod method, const std::uint8_t* packed, std::uint64_t packedSize,
	         std::uint64_t size, std::uint32_t crc, ZipEntry* added);
	// Escribe el directorio central y cierra.  false si algo falló en el camino.
	bool Finish();

	std::uint64_t Position() const { return m_Pos; }

private:
	bool Write(const void* data, std::size_t size);

	FILE* m_File = nullptr;
	std::uint64_t m_Pos = 0;
	bool m_Failed = false;
	std::vector<ZipEntry> m_Entries;
};

class ZipReader
{
public:
	// Lee el directorio central y la cabecera local de cada entrada (para sacar
	// dataOffset).  false si no es un zip válido, si está cifrado o si una
	// entrada se sale del archivo.
	bool Open(const std::string& path, std::string* error);

	const std::vector<ZipEntry>& Entries() const { return m_Entries; }
	const ZipEntry* Find(const std::string& name) const;

private:
	std::vector<ZipEntry> m_Entries;
};

// Lee los datos de una entrada desde su offset y los descomprime; comprueba
// tamaño y CRC.
bool ZipExtract(const std::string& zipPath, const ZipEntry& entry, std::vector<std::uint8_t>& out,
                std::string* error);

// Igual, para datos ya leídos (lo que baja el launcher por Range).
bool ZipDecode(ZipMethod method, const std::uint8_t* packed, std::size_t packedSize,
               std::uint64_t size, std::vector<std::uint8_t>& out, std::string* error);

} // namespace Update
