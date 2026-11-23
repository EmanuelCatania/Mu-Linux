#include "update/Delta.h"

#include <limits>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace Update {

namespace {

// Ventana de zstd que cubre size bytes (potencia de 2 >= size).
int WindowLogFor(std::uint64_t size)
{
	int log = ZSTD_WINDOWLOG_MIN;
	while (log < ZSTD_WINDOWLOG_MAX && (1ull << log) < size) ++log;
	return log;
}

bool Fail(std::string* error, const std::string& what)
{
	if (error) *error = what;
	return false;
}

bool CheckZstd(std::size_t result, std::string* error, const char* where)
{
	if (!ZSTD_isError(result)) return true;
	return Fail(error, std::string(where) + ": " + ZSTD_getErrorName(result));
}

bool FitsInMemory(std::uint64_t size)
{
	return size <= static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / 2);
}

bool Decompress(ZSTD_DCtx* d, const std::uint8_t* data, std::size_t size, std::uint64_t expectedSize,
                std::vector<std::uint8_t>& out, std::string* error)
{
	if (!FitsInMemory(expectedSize)) return Fail(error, "archivo demasiado grande");
	// El tamaño lo dice el frame; si no coincide con el manifiesto, se rechaza
	// antes de reservar memoria.
	const unsigned long long frameSize = ZSTD_getFrameContentSize(data, size);
	if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN && frameSize != expectedSize)
		return Fail(error, "el tamaño del frame zstd no coincide con el del manifiesto");
	out.resize(static_cast<std::size_t>(expectedSize));
	const std::size_t got = ZSTD_decompressDCtx(d, out.data(), out.size(), data, size);
	if (!CheckZstd(got, error, "zstd")) return false;
	if (got != expectedSize) return Fail(error, "el archivo descomprimido no tiene el tamaño esperado");
	return true;
}

} // namespace

bool ZstdCompress(const std::uint8_t* data, std::size_t size, int level,
                  std::vector<std::uint8_t>& out, std::string* error)
{
	ZSTD_CCtx* c = ZSTD_createCCtx();
	if (!c) return Fail(error, "sin memoria para zstd");
	bool ok = CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level), error, "nivel") &&
	          CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1), error, "checksum") &&
	          CheckZstd(ZSTD_CCtx_setPledgedSrcSize(c, size), error, "tamaño");
	if (ok)
	{
		out.resize(ZSTD_compressBound(size));
		const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), data, size);
		ok = CheckZstd(n, error, "zstd");
		if (ok) out.resize(n);
	}
	ZSTD_freeCCtx(c);
	return ok;
}

bool ZstdDecompress(const std::uint8_t* data, std::size_t size, std::uint64_t expectedSize,
                    std::vector<std::uint8_t>& out, std::string* error)
{
	ZSTD_DCtx* d = ZSTD_createDCtx();
	if (!d) return Fail(error, "sin memoria para zstd");
	const bool ok = Decompress(d, data, size, expectedSize, out, error);
	ZSTD_freeDCtx(d);
	return ok;
}

bool DeltaCreate(const std::vector<std::uint8_t>& oldData, const std::vector<std::uint8_t>& newData,
                 int level, std::vector<std::uint8_t>& out, std::string* error)
{
	if (oldData.size() > kMaxDeltaFileSize || newData.size() > kMaxDeltaFileSize)
		return Fail(error, "archivo demasiado grande para un delta");
	ZSTD_CCtx* c = ZSTD_createCCtx();
	if (!c) return Fail(error, "sin memoria para zstd");
	const std::uint64_t largest = oldData.size() > newData.size() ? oldData.size() : newData.size();
	bool ok = CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, level), error, "nivel") &&
	          CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, WindowLogFor(largest)), error, "ventana") &&
	          CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1), error, "ldm") &&
	          CheckZstd(ZSTD_CCtx_setParameter(c, ZSTD_c_checksumFlag, 1), error, "checksum") &&
	          CheckZstd(ZSTD_CCtx_setPledgedSrcSize(c, newData.size()), error, "tamaño") &&
	          CheckZstd(ZSTD_CCtx_refPrefix(c, oldData.data(), oldData.size()), error, "prefijo");
	if (ok)
	{
		out.resize(ZSTD_compressBound(newData.size()));
		const std::size_t n = ZSTD_compress2(c, out.data(), out.size(), newData.data(), newData.size());
		ok = CheckZstd(n, error, "delta");
		if (ok) out.resize(n);
	}
	ZSTD_freeCCtx(c);
	return ok;
}

bool DeltaApply(const std::vector<std::uint8_t>& oldData, const std::uint8_t* patch, std::size_t patchSize,
                std::uint64_t expectedSize, std::vector<std::uint8_t>& out, std::string* error)
{
	if (oldData.size() > kMaxDeltaFileSize || expectedSize > kMaxDeltaFileSize)
		return Fail(error, "archivo demasiado grande para un delta");
	ZSTD_DCtx* d = ZSTD_createDCtx();
	if (!d) return Fail(error, "sin memoria para zstd");
	const std::uint64_t largest = oldData.size() > expectedSize ? oldData.size() : expectedSize;
	bool ok = CheckZstd(ZSTD_DCtx_setParameter(d, ZSTD_d_windowLogMax, WindowLogFor(largest)), error, "ventana") &&
	          CheckZstd(ZSTD_DCtx_refPrefix(d, oldData.data(), oldData.size()), error, "prefijo");
	if (ok) ok = Decompress(d, patch, patchSize, expectedSize, out, error);
	ZSTD_freeDCtx(d);
	return ok;
}

} // namespace Update
