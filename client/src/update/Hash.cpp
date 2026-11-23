#include "update/Hash.h"

#include "update/FileUtil.h"

#include <cstdio>
#include <new>
#include <vector>

extern "C" {
#include "net/crypto/monocypher.h"
}

namespace Update {

namespace {
crypto_blake2b_ctx* Ctx(std::uint8_t* raw) { return reinterpret_cast<crypto_blake2b_ctx*>(raw); }
const char kDigestPrefix[] = "blake2b-256:";
}

static_assert(sizeof(crypto_blake2b_ctx) <= 256, "Hasher::m_Ctx es chico para crypto_blake2b_ctx");

Hasher::Hasher()
{
	crypto_blake2b_init(new (m_Ctx) crypto_blake2b_ctx, 32);
}

void Hasher::Update(const void* data, std::size_t size)
{
	crypto_blake2b_update(Ctx(m_Ctx), static_cast<const std::uint8_t*>(data), size);
}

Digest Hasher::Final()
{
	Digest d;
	crypto_blake2b_final(Ctx(m_Ctx), d.data());
	return d;
}

Digest HashBytes(const void* data, std::size_t size)
{
	Hasher h;
	h.Update(data, size);
	return h.Final();
}

bool HashFile(const std::string& path, Digest& out, std::uint64_t* size)
{
	FILE* f = OpenFile(path, "rb");
	if (!f) return false;
	Hasher h;
	std::vector<std::uint8_t> buf(1 << 20);
	std::uint64_t total = 0;
	for (;;)
	{
		const std::size_t got = std::fread(buf.data(), 1, buf.size(), f);
		if (got) h.Update(buf.data(), got);
		total += got;
		if (got < buf.size()) break;
	}
	const bool ok = std::ferror(f) == 0;
	std::fclose(f);
	if (!ok) return false;
	out = h.Final();
	if (size) *size = total;
	return true;
}

std::string ToHex(const std::uint8_t* data, std::size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string s;
	s.reserve(size * 2);
	for (std::size_t i = 0; i < size; ++i)
	{
		s += digits[data[i] >> 4];
		s += digits[data[i] & 15];
	}
	return s;
}

bool FromHex(const std::string& text, std::uint8_t* out, std::size_t size)
{
	if (text.size() != size * 2) return false;
	for (std::size_t i = 0; i < size; ++i)
	{
		int v = 0;
		for (int k = 0; k < 2; ++k)
		{
			const char c = text[i * 2 + k];
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		out[i] = static_cast<std::uint8_t>(v);
	}
	return true;
}

std::string FormatDigest(const Digest& d)
{
	return kDigestPrefix + ToHex(d.data(), d.size());
}

bool ParseDigest(const std::string& text, Digest& out)
{
	const std::size_t n = sizeof(kDigestPrefix) - 1;
	if (text.compare(0, n, kDigestPrefix) != 0) return false;
	return FromHex(text.substr(n), out.data(), out.size());
}

} // namespace Update
