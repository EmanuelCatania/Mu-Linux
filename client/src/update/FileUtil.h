#pragma once

// Archivos para el actualizador.  Las rutas relativas del manifiesto van siempre
// con "/" y en UTF-8; acá se convierten a las del sistema.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Update {

// fopen con rutas UTF-8 también en Windows, y seek de 64 bits.
FILE* OpenFile(const std::string& path, const char* mode);
bool SeekFile(FILE* f, std::uint64_t offset);

bool ReadWholeFile(const std::string& path, std::vector<std::uint8_t>& out);
bool ReadWholeFile(const std::string& path, std::string& out);

// Lee size bytes desde offset.  false si el archivo es más corto.
bool ReadFileRange(const std::string& path, std::uint64_t offset, std::uint64_t size,
                   std::vector<std::uint8_t>& out);

// Escribe a "<path>.tmp" y lo mueve encima: quien lea el archivo ve el viejo o el
// nuevo, nunca uno a medias.  Crea las carpetas que falten.
bool WriteFileAtomic(const std::string& path, const void* data, std::size_t size);

bool MakeDirs(const std::string& path);
bool FileExists(const std::string& path);
bool FileSize(const std::string& path, std::uint64_t& size);
bool CopyFileTo(const std::string& from, const std::string& to);

// Une una carpeta con una ruta relativa del manifiesto ("a/b.txt").
std::string JoinPath(const std::string& dir, const std::string& rel);

// Lista los archivos de root (recursivo), como rutas relativas con "/",
// ordenadas por bytes.  Ignora enlaces simbólicos.
bool ListFiles(const std::string& root, std::vector<std::string>& out, std::string* error);

// Ruta del manifiesto válida: relativa, con "/", sin "." ni "..", sin
// segmentos vacíos, sin "\", ":" ni caracteres de control, de hasta 512 bytes.
bool IsSafeRelativePath(const std::string& path);

} // namespace Update
