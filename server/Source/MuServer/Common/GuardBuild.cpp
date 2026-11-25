// Ver GuardBuild.h.  Sin stdafx.h: lo usa mu-guard (y su modo `build`).

#include "GuardBuild.h"
#include "GuardProtocol.h"

extern "C"
{
#include "monocypher/monocypher.h"
}

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace
{
	const char kMagic[8] = { 'M', 'U', 'G', 'B', '1', 0, 0, 0 };

	constexpr std::uint32_t kPeExecute = 0x20000000;
	constexpr std::uint32_t kElfLoad = 1;
	constexpr std::uint32_t kElfExecute = 1;

	std::uint16_t U16(const std::vector<std::uint8_t>& b, std::size_t at)
	{
		return at + 2 <= b.size() ? static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)) : 0;
	}

	std::uint32_t U32(const std::vector<std::uint8_t>& b, std::size_t at)
	{
		return at + 4 <= b.size() ? GuardProto::GetLe32(b.data() + at) : 0;
	}

	std::uint64_t U64(const std::vector<std::uint8_t>& b, std::size_t at)
	{
		return at + 8 <= b.size() ? GuardProto::GetLe64(b.data() + at) : 0;
	}

	bool ReadAll(const std::string& path, std::vector<std::uint8_t>& out)
	{
		FILE* file = std::fopen(path.c_str(), "rb");

		if (file == nullptr)
		{
			return false;
		}

		std::uint8_t buffer[1 << 16];

		std::size_t n;

		while ((n = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
		{
			out.insert(out.end(), buffer, buffer + n);
		}

		std::fclose(file);

		return true;
	}

	void Fail(std::string* error, const char* why)
	{
		if (error != nullptr)
		{
			*error = why;
		}
	}

	void Put32(std::vector<std::uint8_t>& out, std::uint32_t v)
	{
		std::uint8_t b[4];
		GuardProto::PutLe32(b, v);
		out.insert(out.end(), b, b + 4);
	}
}

bool CGuardBuild::FromExecutable(const std::string& path, std::string* error)
{
	std::vector<std::uint8_t> file;

	if (!ReadAll(path, file) || file.size() < 64)
	{
		Fail(error, "no se pudo leer el archivo");

		return false;
	}

	crypto_blake2b(this->m_Hash, sizeof(this->m_Hash), file.data(), file.size());

	this->m_Sections.clear();

	this->m_Relocs.clear();

	if (file[0] == 'M' && file[1] == 'Z')
	{
		return this->ReadPe(file, error);
	}

	if (file[0] == 0x7F && file[1] == 'E' && file[2] == 'L' && file[3] == 'F')
	{
		return this->ReadElf(file, error);
	}

	Fail(error, "no es un ejecutable PE ni ELF");

	return false;
}

bool CGuardBuild::ReadPe(const std::vector<std::uint8_t>& b, std::string* error)
{
	const std::uint32_t pe = U32(b, 0x3C);

	if (U32(b, pe) != 0x00004550)   // "PE\0\0"
	{
		Fail(error, "cabecera PE inválida");

		return false;
	}

	const std::uint16_t sections = U16(b, pe + 6);

	const std::uint16_t optionalSize = U16(b, pe + 20);

	const std::size_t opt = pe + 24;

	const std::uint16_t magic = U16(b, opt);

	if (magic != 0x10B && magic != 0x20B)
	{
		Fail(error, "cabecera opcional PE desconocida");

		return false;
	}

	const bool pe64 = magic == 0x20B;

	this->m_PreferredBase = pe64 ? U64(b, opt + 24) : U32(b, opt + 28);

	const std::size_t directories = opt + (pe64 ? 112 : 96);

	const std::uint32_t relocRva = U32(b, directories + 5 * 8);

	const std::uint32_t relocSize = U32(b, directories + 5 * 8 + 4);

	struct Header { std::uint32_t va, vsize, rawSize, rawOffset, flags; };

	std::vector<Header> headers;

	for (std::uint16_t i = 0; i < sections; i++)
	{
		const std::size_t at = opt + optionalSize + static_cast<std::size_t>(i) * 40;

		Header h { U32(b, at + 12), U32(b, at + 8), U32(b, at + 16), U32(b, at + 20), U32(b, at + 36) };

		headers.push_back(h);

		if ((h.flags & kPeExecute) == 0 || h.vsize == 0)
		{
			continue;
		}

		GuardBuildSection section;

		section.Rva = h.va;

		section.Bytes.assign(h.vsize, 0);

		const std::uint32_t copy = std::min(h.vsize, h.rawSize);

		if (h.rawOffset + static_cast<std::size_t>(copy) > b.size())
		{
			Fail(error, "sección fuera del archivo");

			return false;
		}

		std::memcpy(section.Bytes.data(), b.data() + h.rawOffset, copy);

		this->m_Sections.push_back(std::move(section));
	}

	if (this->m_Sections.empty())
	{
		Fail(error, "sin secciones de código");

		return false;
	}

	auto rvaToOffset = [&](std::uint32_t rva) -> std::size_t
	{
		for (const Header& h : headers)
		{
			if (rva >= h.va && rva - h.va < h.rawSize)
			{
				return h.rawOffset + (rva - h.va);
			}
		}

		return 0;
	};

	auto inCode = [&](std::uint32_t rva)
	{
		for (const GuardBuildSection& s : this->m_Sections)
		{
			if (rva >= s.Rva && rva - s.Rva < s.Bytes.size())
			{
				return true;
			}
		}

		return false;
	};

	// Bloques de relocaciones: página (4) | largo del bloque (4) | entradas de 2
	// bytes (tipo en los 4 bits altos, desplazamiento en los 12 bajos).
	const std::size_t start = relocRva != 0 ? rvaToOffset(relocRva) : 0;

	for (std::size_t at = start; start != 0 && at + 8 <= start + relocSize && at + 8 <= b.size();)
	{
		const std::uint32_t page = U32(b, at);

		const std::uint32_t blockSize = U32(b, at + 4);

		if (blockSize < 8)
		{
			break;
		}

		for (std::size_t e = at + 8; e + 2 <= at + blockSize && e + 2 <= b.size(); e += 2)
		{
			const std::uint16_t entry = U16(b, e);

			const int type = entry >> 12;

			const std::uint32_t rva = page + (entry & 0xFFF);

			if ((type == 3 || type == 10) && inCode(rva))
			{
				this->m_Relocs.push_back({ rva, static_cast<std::uint8_t>(type == 10 ? 8 : 4) });
			}
		}

		at += blockSize;
	}

	std::sort(this->m_Relocs.begin(), this->m_Relocs.end(), [](const GuardBuildReloc& a, const GuardBuildReloc& c) { return a.Rva < c.Rva; });

	return true;
}

bool CGuardBuild::ReadElf(const std::vector<std::uint8_t>& b, std::string* error)
{
	const bool elf64 = b[4] == 2;

	if ((b[4] != 1 && b[4] != 2) || b[5] != 1)
	{
		Fail(error, "ELF no soportado (sólo little-endian)");

		return false;
	}

	const std::uint64_t phoff = elf64 ? U64(b, 0x20) : U32(b, 0x1C);

	const std::uint16_t phentsize = elf64 ? U16(b, 0x36) : U16(b, 0x2A);

	const std::uint16_t phnum = elf64 ? U16(b, 0x38) : U16(b, 0x2C);

	struct Segment { std::uint32_t type, flags; std::uint64_t offset, vaddr, filesz, memsz; };

	std::vector<Segment> loads;

	std::uint64_t lowest = ~0ULL;

	for (std::uint16_t i = 0; i < phnum; i++)
	{
		const std::size_t at = static_cast<std::size_t>(phoff) + static_cast<std::size_t>(i) * phentsize;

		Segment s {};

		s.type = U32(b, at);

		if (elf64)
		{
			s.flags = U32(b, at + 4);
			s.offset = U64(b, at + 8);
			s.vaddr = U64(b, at + 16);
			s.filesz = U64(b, at + 32);
			s.memsz = U64(b, at + 40);
		}
		else
		{
			s.offset = U32(b, at + 4);
			s.vaddr = U32(b, at + 8);
			s.filesz = U32(b, at + 16);
			s.memsz = U32(b, at + 20);
			s.flags = U32(b, at + 24);
		}

		if (s.type == kElfLoad)
		{
			loads.push_back(s);

			lowest = std::min(lowest, s.vaddr);
		}
	}

	// La base es donde queda la primera página del primer segmento.
	this->m_PreferredBase = lowest & ~0xFFFULL;

	for (const Segment& s : loads)
	{
		if ((s.flags & kElfExecute) == 0 || s.memsz == 0)
		{
			continue;
		}

		if (s.offset + s.filesz > b.size())
		{
			Fail(error, "segmento fuera del archivo");

			return false;
		}

		GuardBuildSection section;

		section.Rva = static_cast<std::uint32_t>(s.vaddr - this->m_PreferredBase);

		section.Bytes.assign(static_cast<std::size_t>(s.memsz), 0);

		std::memcpy(section.Bytes.data(), b.data() + s.offset, static_cast<std::size_t>(std::min(s.filesz, s.memsz)));

		this->m_Sections.push_back(std::move(section));
	}

	if (this->m_Sections.empty())
	{
		Fail(error, "sin segmentos de código");

		return false;
	}

	return true;
}

bool CGuardBuild::Save(const std::string& path) const
{
	std::vector<std::uint8_t> out(kMagic, kMagic + 8);

	out.insert(out.end(), this->m_Hash, this->m_Hash + 32);

	std::uint8_t base[8];

	GuardProto::PutLe64(base, this->m_PreferredBase);

	out.insert(out.end(), base, base + 8);

	Put32(out, static_cast<std::uint32_t>(this->m_Sections.size()));

	for (const GuardBuildSection& s : this->m_Sections)
	{
		Put32(out, s.Rva);

		Put32(out, static_cast<std::uint32_t>(s.Bytes.size()));

		out.insert(out.end(), s.Bytes.begin(), s.Bytes.end());
	}

	Put32(out, static_cast<std::uint32_t>(this->m_Relocs.size()));

	for (const GuardBuildReloc& r : this->m_Relocs)
	{
		Put32(out, r.Rva);

		out.push_back(r.Width);
	}

	FILE* file = std::fopen(path.c_str(), "wb");

	if (file == nullptr)
	{
		return false;
	}

	const bool ok = std::fwrite(out.data(), 1, out.size(), file) == out.size();

	return std::fclose(file) == 0 && ok;
}

bool CGuardBuild::Load(const std::string& path)
{
	std::vector<std::uint8_t> b;

	if (!ReadAll(path, b) || b.size() < 8 + 32 + 8 + 4 || std::memcmp(b.data(), kMagic, 8) != 0)
	{
		return false;
	}

	std::memcpy(this->m_Hash, b.data() + 8, 32);

	this->m_PreferredBase = U64(b, 40);

	this->m_Sections.clear();

	this->m_Relocs.clear();

	std::size_t at = 48;

	const std::uint32_t sections = U32(b, at);

	at += 4;

	for (std::uint32_t i = 0; i < sections; i++)
	{
		const std::uint32_t rva = U32(b, at), size = U32(b, at + 4);

		at += 8;

		if (at + size > b.size())
		{
			return false;
		}

		GuardBuildSection s;

		s.Rva = rva;

		s.Bytes.assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + size));

		this->m_Sections.push_back(std::move(s));

		at += size;
	}

	const std::uint32_t relocs = U32(b, at);

	at += 4;

	if (at + static_cast<std::size_t>(relocs) * 5 > b.size())
	{
		return false;
	}

	for (std::uint32_t i = 0; i < relocs; i++, at += 5)
	{
		this->m_Relocs.push_back({ U32(b, at), b[at + 4] });
	}

	return !this->m_Sections.empty();
}

std::vector<GuardBuildRange> CGuardBuild::Chunks(std::uint32_t size) const
{
	std::vector<GuardBuildRange> chunks;

	for (const GuardBuildSection& s : this->m_Sections)
	{
		const std::uint32_t length = static_cast<std::uint32_t>(s.Bytes.size());

		for (std::uint32_t at = 0; at < length; at += size)
		{
			chunks.push_back({ s.Rva + at, std::min(size, length - at) });
		}
	}

	return chunks;
}

bool CGuardBuild::Copy(std::uint32_t rva, std::uint32_t size, std::uint64_t base, std::vector<std::uint8_t>& out) const
{
	for (const GuardBuildSection& s : this->m_Sections)
	{
		if (rva < s.Rva || static_cast<std::uint64_t>(rva - s.Rva) + size > s.Bytes.size())
		{
			continue;
		}

		const std::size_t at = out.size();

		out.insert(out.end(), s.Bytes.begin() + (rva - s.Rva), s.Bytes.begin() + (rva - s.Rva) + size);

		// Las palabras que corrige el cargador (incluida una que empiece hasta 7
		// bytes antes del rango y se meta en él).
		const std::uint64_t delta = base - this->m_PreferredBase;

		auto first = std::lower_bound(this->m_Relocs.begin(), this->m_Relocs.end(), rva >= 7 ? rva - 7 : 0,
			[](const GuardBuildReloc& r, std::uint32_t value) { return r.Rva < value; });

		for (auto it = first; it != this->m_Relocs.end() && it->Rva < rva + size; ++it)
		{
			if (it->Rva < s.Rva || it->Rva - s.Rva + it->Width > s.Bytes.size())
			{
				continue;
			}

			std::uint8_t word[8] = {};

			std::memcpy(word, s.Bytes.data() + (it->Rva - s.Rva), it->Width);

			if (it->Width == 8)
			{
				GuardProto::PutLe64(word, GuardProto::GetLe64(word) + delta);
			}
			else
			{
				GuardProto::PutLe32(word, GuardProto::GetLe32(word) + static_cast<std::uint32_t>(delta));
			}

			for (std::uint8_t k = 0; k < it->Width; k++)
			{
				const std::uint64_t target = static_cast<std::uint64_t>(it->Rva) + k;

				if (target >= rva && target < static_cast<std::uint64_t>(rva) + size)
				{
					out[at + static_cast<std::size_t>(target - rva)] = word[k];
				}
			}
		}

		return true;
	}

	return false;
}

bool CGuardBuild::Expected(const std::uint8_t nonce[16], const std::vector<GuardBuildRange>& ranges, std::uint64_t base,
                           std::uint8_t out[32]) const
{
	std::vector<std::uint8_t> bytes;

	for (const GuardBuildRange& r : ranges)
	{
		if (!this->Copy(r.Rva, r.Size, base, bytes))
		{
			return false;
		}
	}

	crypto_blake2b_ctx ctx;

	crypto_blake2b_keyed_init(&ctx, 32, nonce, 16);

	crypto_blake2b_update(&ctx, reinterpret_cast<const std::uint8_t*>(GuardProto::ChallengeContext), sizeof(GuardProto::ChallengeContext) - 1);

	crypto_blake2b_update(&ctx, bytes.data(), bytes.size());

	crypto_blake2b_final(&ctx, out);

	return true;
}
