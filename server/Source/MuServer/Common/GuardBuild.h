#pragma once

// Builds aceptados de MU-Guard (paso 4 del diseño): lo que mu-guard necesita de
// un ejecutable del cliente para verificar el desafío de memoria sin tener el
// ejecutable entero.
//
// De un main.exe (PE de 32 o 64 bits) o un main de Linux (ELF) se guarda:
//   - el hash BLAKE2b-256 del archivo (el guardián lo manda en el saludo);
//   - las secciones de código (ejecutables), con su RVA y sus bytes tal como
//     quedan en memoria;
//   - las relocaciones de Windows que caen en esas secciones: el juego carga en
//     otra dirección (ASLR) y el cargador corrige esas palabras.  En Linux el
//     código es independiente de la posición y no hay.
//
// Archivo .guardbuild (lo arma `mu-guard build`, little-endian):
//   "MUGB1\0\0\0" | hash (32) | base preferida (8) | secciones (4)
//   | por sección: RVA (4) | largo (4) | bytes
//   | relocaciones (4) | por relocación: RVA (4) | ancho (1: 4 u 8)

#include <cstdint>
#include <string>
#include <vector>

struct GuardBuildSection
{
	std::uint32_t Rva = 0;
	std::vector<std::uint8_t> Bytes;
};

struct GuardBuildReloc
{
	std::uint32_t Rva = 0;
	std::uint8_t Width = 0;   // 4 (PE32) u 8 (PE32+)
};

struct GuardBuildRange
{
	std::uint32_t Rva = 0;
	std::uint32_t Size = 0;
};

class CGuardBuild
{
public:

	// Lee un ejecutable.  false y el motivo en *error si no es un PE o ELF que se
	// entienda.
	bool FromExecutable(const std::string& path, std::string* error);

	bool Save(const std::string& path) const;

	bool Load(const std::string& path);

	const std::uint8_t* Hash() const { return this->m_Hash; }

	const std::vector<GuardBuildSection>& Sections() const { return this->m_Sections; }

	// Todo el código partido en bloques de `size` bytes (el último de cada
	// sección puede ser más chico).  mu-guard los pide en orden aleatorio.
	std::vector<GuardBuildRange> Chunks(std::uint32_t size) const;

	// Hash esperado de un desafío: los bytes de los rangos como quedan en el
	// juego cargado en `base`.  false si algún rango cae fuera del código.
	bool Expected(const std::uint8_t nonce[16], const std::vector<GuardBuildRange>& ranges, std::uint64_t base,
	              std::uint8_t out[32]) const;

private:

	bool ReadPe(const std::vector<std::uint8_t>& file, std::string* error);

	bool ReadElf(const std::vector<std::uint8_t>& file, std::string* error);

	// Copia [rva, rva+size) con las relocaciones aplicadas para `base`.
	bool Copy(std::uint32_t rva, std::uint32_t size, std::uint64_t base, std::vector<std::uint8_t>& out) const;

	std::uint8_t m_Hash[32] = {};

	std::uint64_t m_PreferredBase = 0;

	std::vector<GuardBuildSection> m_Sections;

	std::vector<GuardBuildReloc> m_Relocs;   // ordenadas por RVA
};
