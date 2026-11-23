#include "update/Zip.h"

#include "update/Delta.h"
#include "update/FileUtil.h"

#include <cstring>

namespace Update {

namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEndSig = 0x06054b50;
constexpr std::uint32_t kZip64EndSig = 0x06064b50;
constexpr std::uint32_t kZip64LocatorSig = 0x07064b50;
constexpr std::uint32_t k32 = 0xFFFFFFFFu;
constexpr std::uint16_t kFlagUtf8 = 0x0800;
// Fecha fija (1980-01-01 00:00): el mismo contenido da el mismo zip.
constexpr std::uint16_t kDosDate = 0x0021;
// Tope del directorio central que se acepta leer (unos 500.000 archivos).
constexpr std::uint64_t kMaxCentralDir = 64ull << 20;

void Put16(std::vector<std::uint8_t>& b, std::uint16_t v)
{
	b.push_back(static_cast<std::uint8_t>(v));
	b.push_back(static_cast<std::uint8_t>(v >> 8));
}

void Put32(std::vector<std::uint8_t>& b, std::uint32_t v)
{
	for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void Put64(std::vector<std::uint8_t>& b, std::uint64_t v)
{
	for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint16_t Get16(const std::uint8_t* p) { return static_cast<std::uint16_t>(p[0] | (p[1] << 8)); }

std::uint32_t Get32(const std::uint8_t* p)
{
	return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
	       (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t Get64(const std::uint8_t* p)
{
	return static_cast<std::uint64_t>(Get32(p)) | (static_cast<std::uint64_t>(Get32(p + 4)) << 32);
}

std::uint16_t VersionNeeded(ZipMethod method, bool zip64)
{
	if (method == ZipMethod::Zstd) return 63;
	return zip64 ? 45 : 20;
}

bool Fail(std::string* error, const std::string& what)
{
	if (error) *error = what;
	return false;
}

} // namespace

namespace {
struct CrcTable
{
	std::uint32_t v[256];
	CrcTable()
	{
		for (std::uint32_t i = 0; i < 256; ++i)
		{
			std::uint32_t c = i;
			for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			v[i] = c;
		}
	}
};
} // namespace

std::uint32_t Crc32(const void* data, std::size_t size, std::uint32_t crc)
{
	static const CrcTable table;
	const std::uint8_t* p = static_cast<const std::uint8_t*>(data);
	crc = ~crc;
	for (std::size_t i = 0; i < size; ++i) crc = table.v[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

ZipWriter::~ZipWriter()
{
	if (m_File) std::fclose(m_File);
}

bool ZipWriter::Open(const std::string& path)
{
	m_File = OpenFile(path, "wb");
	m_Pos = 0;
	m_Failed = m_File == nullptr;
	m_Entries.clear();
	return m_File != nullptr;
}

bool ZipWriter::Write(const void* data, std::size_t size)
{
	if (m_Failed || !m_File) return false;
	if (size && std::fwrite(data, 1, size, m_File) != size)
	{
		m_Failed = true;
		return false;
	}
	m_Pos += size;
	return true;
}

bool ZipWriter::Add(const std::string& name, ZipMethod method, const std::uint8_t* packed, std::uint64_t packedSize,
                    std::uint64_t size, std::uint32_t crc, ZipEntry* added)
{
	if (name.empty() || name.size() > 0xFFFF)
	{
		m_Failed = true;
		return false;
	}
	const bool zip64 = size >= k32 || packedSize >= k32;
	ZipEntry e;
	e.name = name;
	e.method = method;
	e.crc = crc;
	e.packedSize = packedSize;
	e.size = size;
	e.headerOffset = m_Pos;

	std::vector<std::uint8_t> h;
	Put32(h, kLocalSig);
	Put16(h, VersionNeeded(method, zip64));
	Put16(h, kFlagUtf8);
	Put16(h, static_cast<std::uint16_t>(method));
	Put16(h, 0);
	Put16(h, kDosDate);
	Put32(h, crc);
	Put32(h, zip64 ? k32 : static_cast<std::uint32_t>(packedSize));
	Put32(h, zip64 ? k32 : static_cast<std::uint32_t>(size));
	Put16(h, static_cast<std::uint16_t>(name.size()));
	Put16(h, zip64 ? 20 : 0);
	h.insert(h.end(), name.begin(), name.end());
	if (zip64)
	{
		Put16(h, 0x0001);
		Put16(h, 16);
		Put64(h, size);
		Put64(h, packedSize);
	}
	if (!Write(h.data(), h.size())) return false;
	e.dataOffset = m_Pos;
	if (!Write(packed, static_cast<std::size_t>(packedSize))) return false;
	m_Entries.push_back(e);
	if (added) *added = e;
	return true;
}

bool ZipWriter::Finish()
{
	if (!m_File) return false;
	const std::uint64_t cdStart = m_Pos;
	bool anyZip64 = false;
	for (const ZipEntry& e : m_Entries)
	{
		const bool sizes64 = e.size >= k32 || e.packedSize >= k32;
		const bool offset64 = e.headerOffset >= k32;
		anyZip64 = anyZip64 || sizes64 || offset64;
		std::vector<std::uint8_t> extra;
		if (sizes64 || offset64)
		{
			Put16(extra, 0x0001);
			Put16(extra, static_cast<std::uint16_t>((sizes64 ? 16 : 0) + (offset64 ? 8 : 0)));
			if (sizes64) { Put64(extra, e.size); Put64(extra, e.packedSize); }
			if (offset64) Put64(extra, e.headerOffset);
		}
		std::vector<std::uint8_t> h;
		Put32(h, kCentralSig);
		Put16(h, 63);
		Put16(h, VersionNeeded(e.method, sizes64 || offset64));
		Put16(h, kFlagUtf8);
		Put16(h, static_cast<std::uint16_t>(e.method));
		Put16(h, 0);
		Put16(h, kDosDate);
		Put32(h, e.crc);
		Put32(h, sizes64 ? k32 : static_cast<std::uint32_t>(e.packedSize));
		Put32(h, sizes64 ? k32 : static_cast<std::uint32_t>(e.size));
		Put16(h, static_cast<std::uint16_t>(e.name.size()));
		Put16(h, static_cast<std::uint16_t>(extra.size()));
		Put16(h, 0);
		Put16(h, 0);
		Put16(h, 0);
		Put32(h, 0);
		Put32(h, offset64 ? k32 : static_cast<std::uint32_t>(e.headerOffset));
		h.insert(h.end(), e.name.begin(), e.name.end());
		h.insert(h.end(), extra.begin(), extra.end());
		Write(h.data(), h.size());
	}
	const std::uint64_t cdSize = m_Pos - cdStart;
	const std::uint64_t count = m_Entries.size();
	std::vector<std::uint8_t> t;
	if (anyZip64 || count >= 0xFFFF || cdStart >= k32 || cdSize >= k32)
	{
		const std::uint64_t zip64End = m_Pos;
		Put32(t, kZip64EndSig);
		Put64(t, 44);
		Put16(t, 63);
		Put16(t, 45);
		Put32(t, 0);
		Put32(t, 0);
		Put64(t, count);
		Put64(t, count);
		Put64(t, cdSize);
		Put64(t, cdStart);
		Put32(t, kZip64LocatorSig);
		Put32(t, 0);
		Put64(t, zip64End);
		Put32(t, 1);
	}
	Put32(t, kEndSig);
	Put16(t, 0);
	Put16(t, 0);
	Put16(t, count >= 0xFFFF ? 0xFFFF : static_cast<std::uint16_t>(count));
	Put16(t, count >= 0xFFFF ? 0xFFFF : static_cast<std::uint16_t>(count));
	Put32(t, cdSize >= k32 ? k32 : static_cast<std::uint32_t>(cdSize));
	Put32(t, cdStart >= k32 ? k32 : static_cast<std::uint32_t>(cdStart));
	Put16(t, 0);
	Write(t.data(), t.size());
	const bool ok = !m_Failed && std::fflush(m_File) == 0;
	const bool closed = std::fclose(m_File) == 0;
	m_File = nullptr;
	return ok && closed;
}

bool ZipReader::Open(const std::string& path, std::string* error)
{
	m_Entries.clear();
	std::uint64_t fileSize = 0;
	if (!FileSize(path, fileSize)) return Fail(error, "no se puede leer " + path);
	if (fileSize < 22) return Fail(error, "no es un zip: " + path);

	const std::uint64_t tailSize = fileSize < 22 + 0xFFFF ? fileSize : 22 + 0xFFFF;
	std::vector<std::uint8_t> tail;
	if (!ReadFileRange(path, fileSize - tailSize, tailSize, tail)) return Fail(error, "no se puede leer " + path);

	std::size_t eocd = std::string::npos;
	for (std::size_t i = tail.size() - 22 + 1; i-- > 0;)
	{
		if (Get32(&tail[i]) == kEndSig && i + 22 + Get16(&tail[i + 20]) == tail.size())
		{
			eocd = i;
			break;
		}
	}
	if (eocd == std::string::npos) return Fail(error, "no es un zip (falta el final del directorio): " + path);

	const std::uint8_t* e = &tail[eocd];
	if (Get16(e + 4) != 0 || Get16(e + 6) != 0) return Fail(error, "zip partido en varios discos: " + path);
	std::uint64_t count = Get16(e + 10);
	std::uint64_t cdSize = Get32(e + 12);
	std::uint64_t cdStart = Get32(e + 16);
	const std::uint64_t eocdPos = fileSize - tailSize + eocd;

	if (count == 0xFFFF || cdSize == k32 || cdStart == k32)
	{
		if (eocdPos < 20) return Fail(error, "zip64 sin localizador: " + path);
		std::vector<std::uint8_t> loc;
		if (!ReadFileRange(path, eocdPos - 20, 20, loc) || Get32(loc.data()) != kZip64LocatorSig)
			return Fail(error, "zip64 sin localizador: " + path);
		const std::uint64_t zip64End = Get64(loc.data() + 8);
		std::vector<std::uint8_t> rec;
		if (zip64End + 56 > eocdPos || !ReadFileRange(path, zip64End, 56, rec) || Get32(rec.data()) != kZip64EndSig)
			return Fail(error, "zip64 con el final dañado: " + path);
		count = Get64(rec.data() + 32);
		cdSize = Get64(rec.data() + 40);
		cdStart = Get64(rec.data() + 48);
	}
	if (cdSize > kMaxCentralDir || cdStart + cdSize > eocdPos || count > cdSize / 46)
		return Fail(error, "directorio del zip fuera de rango: " + path);

	std::vector<std::uint8_t> cd;
	if (!ReadFileRange(path, cdStart, cdSize, cd)) return Fail(error, "no se puede leer el directorio de " + path);

	std::size_t p = 0;
	m_Entries.reserve(static_cast<std::size_t>(count));
	for (std::uint64_t i = 0; i < count; ++i)
	{
		if (p + 46 > cd.size() || Get32(&cd[p]) != kCentralSig) return Fail(error, "directorio del zip dañado: " + path);
		const std::uint8_t* h = &cd[p];
		const std::uint16_t flags = Get16(h + 8);
		if (flags & 0x0041) return Fail(error, "zip cifrado (no se aceptan): " + path);
		ZipEntry z;
		z.method = static_cast<ZipMethod>(Get16(h + 10));
		z.crc = Get32(h + 16);
		z.packedSize = Get32(h + 20);
		z.size = Get32(h + 24);
		const std::size_t nameLen = Get16(h + 28), extraLen = Get16(h + 30), commentLen = Get16(h + 32);
		z.headerOffset = Get32(h + 42);
		if (p + 46 + nameLen + extraLen + commentLen > cd.size()) return Fail(error, "directorio del zip dañado: " + path);
		z.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);

		// Zip64: sólo están los campos que en la cabecera valen 0xFFFFFFFF, en orden.
		const std::uint8_t* x = h + 46 + nameLen;
		const std::uint8_t* xEnd = x + extraLen;
		while (x + 4 <= xEnd)
		{
			const std::uint16_t id = Get16(x), len = Get16(x + 2);
			if (x + 4 + len > xEnd) break;
			if (id == 0x0001)
			{
				const std::uint8_t* f = x + 4;
				const std::uint8_t* fEnd = f + len;
				if (z.size == k32) { if (f + 8 > fEnd) return Fail(error, "zip64 dañado: " + path); z.size = Get64(f); f += 8; }
				if (z.packedSize == k32) { if (f + 8 > fEnd) return Fail(error, "zip64 dañado: " + path); z.packedSize = Get64(f); f += 8; }
				if (z.headerOffset == k32) { if (f + 8 > fEnd) return Fail(error, "zip64 dañado: " + path); z.headerOffset = Get64(f); }
			}
			x += 4 + len;
		}
		p += 46 + nameLen + extraLen + commentLen;
		m_Entries.push_back(std::move(z));
	}

	FILE* f = OpenFile(path, "rb");
	if (!f) return Fail(error, "no se puede leer " + path);
	for (ZipEntry& z : m_Entries)
	{
		std::uint8_t h[30];
		if (z.headerOffset + 30 > cdStart || !SeekFile(f, z.headerOffset) || std::fread(h, 1, 30, f) != 30 ||
		    Get32(h) != kLocalSig)
		{
			std::fclose(f);
			return Fail(error, "cabecera local dañada en " + path + ": " + z.name);
		}
		z.dataOffset = z.headerOffset + 30 + Get16(h + 26) + Get16(h + 28);
		if (z.dataOffset + z.packedSize > cdStart)
		{
			std::fclose(f);
			return Fail(error, "entrada fuera del zip en " + path + ": " + z.name);
		}
	}
	std::fclose(f);
	return true;
}

const ZipEntry* ZipReader::Find(const std::string& name) const
{
	for (const ZipEntry& e : m_Entries)
		if (e.name == name) return &e;
	return nullptr;
}

bool ZipDecode(ZipMethod method, const std::uint8_t* packed, std::size_t packedSize,
               std::uint64_t size, std::vector<std::uint8_t>& out, std::string* error)
{
	switch (method)
	{
	case ZipMethod::Store:
		if (packedSize != size) return Fail(error, "entrada guardada con tamaños distintos");
		out.assign(packed, packed + packedSize);
		return true;
	case ZipMethod::Zstd:
		return ZstdDecompress(packed, packedSize, size, out, error);
	case ZipMethod::Deflate:
		return Fail(error, "Deflate todavía no está soportado");
	}
	return Fail(error, "método de compresión no soportado");
}

bool ZipExtract(const std::string& zipPath, const ZipEntry& entry, std::vector<std::uint8_t>& out,
                std::string* error)
{
	std::vector<std::uint8_t> packed;
	if (!ReadFileRange(zipPath, entry.dataOffset, entry.packedSize, packed))
		return Fail(error, "no se puede leer " + entry.name + " de " + zipPath);
	if (!ZipDecode(entry.method, packed.data(), packed.size(), entry.size, out, error)) return false;
	if (Crc32(out.data(), out.size()) != entry.crc) return Fail(error, "CRC incorrecto en " + entry.name);
	return true;
}

} // namespace Update
