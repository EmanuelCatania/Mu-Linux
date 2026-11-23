#pragma once

// Compresión zstd de los archivos del actualizador y parches binarios
// (el equivalente de "zstd --patch-from": el archivo viejo es el diccionario).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Update {

// Tope de un archivo con delta: la ventana de zstd tiene que cubrir el viejo.
constexpr std::uint64_t kMaxDeltaFileSize = 128ull << 20;

bool ZstdCompress(const std::uint8_t* data, std::size_t size, int level,
                  std::vector<std::uint8_t>& out, std::string* error);

// expectedSize: tamaño del resultado (viene del manifiesto); si no coincide, falla.
bool ZstdDecompress(const std::uint8_t* data, std::size_t size, std::uint64_t expectedSize,
                    std::vector<std::uint8_t>& out, std::string* error);

bool DeltaCreate(const std::vector<std::uint8_t>& oldData, const std::vector<std::uint8_t>& newData,
                 int level, std::vector<std::uint8_t>& out, std::string* error);

bool DeltaApply(const std::vector<std::uint8_t>& oldData, const std::uint8_t* patch, std::size_t patchSize,
                std::uint64_t expectedSize, std::vector<std::uint8_t>& out, std::string* error);

} // namespace Update
