#include "Gfx.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

namespace Launcher {

namespace {

constexpr int kFirst = 32;
constexpr int kCount = 224;     // U+0020..U+00FF

const stbtt_bakedchar* Chars(const std::vector<unsigned char>& v)
{
	return reinterpret_cast<const stbtt_bakedchar*>(v.data());
}

int Glyph(std::uint32_t cp)
{
	if (cp < kFirst || cp >= static_cast<std::uint32_t>(kFirst + kCount)) return '?' - kFirst;
	return static_cast<int>(cp) - kFirst;
}

} // namespace

std::vector<std::uint32_t> DecodeUtf8(const std::string& s)
{
	std::vector<std::uint32_t> out;
	for (std::size_t i = 0; i < s.size();)
	{
		const unsigned char c = static_cast<unsigned char>(s[i]);
		int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
		if (extra < 0 || i + extra >= s.size())
		{
			out.push_back(0xFFFD);
			++i;
			continue;
		}
		std::uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1F) : extra == 2 ? (c & 0x0F) : (c & 0x07);
		bool ok = true;
		for (int k = 1; k <= extra; ++k)
		{
			const unsigned char d = static_cast<unsigned char>(s[i + k]);
			if ((d & 0xC0) != 0x80) { ok = false; break; }
			cp = (cp << 6) | (d & 0x3F);
		}
		out.push_back(ok ? cp : 0xFFFD);
		i += ok ? extra + 1 : 1;
	}
	return out;
}

SDL_Texture* LoadPng(SDL_Renderer* renderer, const unsigned char* data, std::size_t size)
{
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
	if (!px) return nullptr;
	SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
	SDL_Texture* t = s ? SDL_CreateTextureFromSurface(renderer, s) : nullptr;
	if (s) SDL_DestroySurface(s);
	stbi_image_free(px);
	if (t) SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
	return t;
}

SDL_Surface* LoadPngSurface(const unsigned char* data, std::size_t size)
{
	int w = 0, h = 0, n = 0;
	unsigned char* px = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &n, 4);
	if (!px) return nullptr;
	SDL_Surface* s = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_RGBA32, px, w * 4);
	SDL_Surface* copy = s ? SDL_DuplicateSurface(s) : nullptr;
	if (s) SDL_DestroySurface(s);
	stbi_image_free(px);
	return copy;
}

Font::Font(SDL_Renderer* renderer, const unsigned char* ttf, std::size_t, float pixelHeight, float scale)
	: m_Renderer(renderer), m_PixelHeight(pixelHeight), m_Scale(scale < 1.0f ? 1.0f : scale),
	  m_Chars(sizeof(stbtt_bakedchar) * kCount)
{
	const float baked = pixelHeight * m_Scale;
	m_Atlas = baked > 24.0f ? 1024 : 512;
	const int kAtlas = m_Atlas;
	std::vector<unsigned char> gray(static_cast<std::size_t>(kAtlas) * kAtlas);
	if (stbtt_BakeFontBitmap(ttf, 0, baked, gray.data(), kAtlas, kAtlas, kFirst, kCount,
	                         reinterpret_cast<stbtt_bakedchar*>(m_Chars.data())) <= 0)
		return;
	stbtt_fontinfo info;
	if (stbtt_InitFont(&info, ttf, stbtt_GetFontOffsetForIndex(ttf, 0)))
	{
		int ascent = 0, descent = 0, gap = 0;
		stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
		m_Ascent = ascent * stbtt_ScaleForPixelHeight(&info, baked);
	}
	std::vector<std::uint32_t> rgba(static_cast<std::size_t>(kAtlas) * kAtlas);
	for (std::size_t i = 0; i < gray.size(); ++i) rgba[i] = (static_cast<std::uint32_t>(gray[i]) << 24) | 0x00FFFFFFu;
	SDL_Surface* s = SDL_CreateSurfaceFrom(kAtlas, kAtlas, SDL_PIXELFORMAT_ARGB8888, rgba.data(), kAtlas * 4);
	if (s)
	{
		m_Texture = SDL_CreateTextureFromSurface(renderer, s);
		SDL_DestroySurface(s);
	}
	if (m_Texture) SDL_SetTextureBlendMode(m_Texture, SDL_BLENDMODE_BLEND);
}

Font::~Font()
{
	if (m_Texture) SDL_DestroyTexture(m_Texture);
}

float Font::Width(const std::string& utf8) const
{
	float w = 0;
	for (std::uint32_t cp : DecodeUtf8(utf8)) w += Chars(m_Chars)[Glyph(cp)].xadvance;
	return w / m_Scale;
}

void Font::Draw(const std::string& utf8, float x, float y, SDL_Color color) const
{
	if (!m_Texture) return;
	SDL_SetTextureColorMod(m_Texture, color.r, color.g, color.b);
	SDL_SetTextureAlphaMod(m_Texture, color.a);
	// Se avanza en píxeles de la fuente rasterizada y se pasa a unidades lógicas.
	const float a = static_cast<float>(m_Atlas);
	float px = 0, py = m_Ascent;
	for (std::uint32_t cp : DecodeUtf8(utf8))
	{
		stbtt_aligned_quad q;
		stbtt_GetBakedQuad(Chars(m_Chars), m_Atlas, m_Atlas, Glyph(cp), &px, &py, &q, 1);
		const SDL_FRect src{ q.s0 * a, q.t0 * a, (q.s1 - q.s0) * a, (q.t1 - q.t0) * a };
		const SDL_FRect dst{ x + q.x0 / m_Scale, y + q.y0 / m_Scale, (q.x1 - q.x0) / m_Scale, (q.y1 - q.y0) / m_Scale };
		if (dst.w > 0 && dst.h > 0) SDL_RenderTexture(m_Renderer, m_Texture, &src, &dst);
	}
}

void Font::DrawCentered(const std::string& utf8, const SDL_FRect& box, SDL_Color color) const
{
	const float w = Width(utf8);
	Draw(utf8, box.x + (box.w - w) / 2, box.y + (box.h - m_PixelHeight) / 2, color);
}

std::string Font::Fit(const std::string& utf8, float maxWidth) const
{
	if (Width(utf8) <= maxWidth) return utf8;
	std::string s = utf8;
	while (!s.empty() && Width(s + "...") > maxWidth)
	{
		while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
		if (!s.empty()) s.pop_back();
	}
	return s + "...";
}

} // namespace Launcher
