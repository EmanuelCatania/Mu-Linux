#pragma once

// Dibujo del launcher sobre SDL_Renderer: imágenes PNG (stb_image) y texto con
// una fuente TrueType (stb_truetype).  Todo viene de memoria (LauncherRes).
//
// El texto cubre Latin-1 (U+0020..U+00FF): castellano, portugués e inglés.  Un
// carácter fuera de ese rango se dibuja como "?".

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Launcher {

SDL_Texture* LoadPng(SDL_Renderer* renderer, const unsigned char* data, std::size_t size);

class Font
{
public:
	// scale: píxeles de pantalla por unidad lógica.  La fuente se rasteriza a ese
	// tamaño para que el texto no salga borroso al agrandar la ventana; todas las
	// medidas siguen en unidades lógicas.
	Font(SDL_Renderer* renderer, const unsigned char* ttf, std::size_t size, float pixelHeight, float scale = 1.0f);
	~Font();
	Font(const Font&) = delete;
	Font& operator=(const Font&) = delete;

	bool Ok() const { return m_Texture != nullptr; }
	float Width(const std::string& utf8) const;
	float Height() const { return m_PixelHeight; }
	// y es la parte de arriba del texto.
	void Draw(const std::string& utf8, float x, float y, SDL_Color color) const;
	void DrawCentered(const std::string& utf8, const SDL_FRect& box, SDL_Color color) const;
	// Corta con "..." si no entra en maxWidth.
	std::string Fit(const std::string& utf8, float maxWidth) const;

private:
	SDL_Renderer* m_Renderer;
	SDL_Texture* m_Texture = nullptr;
	float m_PixelHeight;
	float m_Scale;
	int m_Atlas;
	float m_Ascent = 0;          // en píxeles de la fuente rasterizada
	std::vector<unsigned char> m_Chars;   // stbtt_bakedchar[224]
};

// Decodifica UTF-8 a puntos de código (los inválidos quedan como U+FFFD).
std::vector<std::uint32_t> DecodeUtf8(const std::string& s);

} // namespace Launcher
