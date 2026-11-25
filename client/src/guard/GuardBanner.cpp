// Ver GuardBanner.h.  Sin stdafx.h: sólo SDL3 y launcher/Gfx.

#include "guard/GuardBanner.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifdef MU_GUARD_BANNER

#include "launcher/Gfx.h"

#include <SDL3/SDL.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace LauncherRes {
extern const unsigned char GuardBackground[];
extern const std::size_t GuardBackgroundSize;
extern const unsigned char GuardLogo[];
extern const std::size_t GuardLogoSize;
extern const unsigned char GuardDragon[];
extern const std::size_t GuardDragonSize;
}

#endif

namespace Guard {

namespace {

using Clock = std::chrono::steady_clock;

} // namespace

#ifdef MU_GUARD_BANNER

namespace {

// Textos del template (TEXTOS), por idioma: castellano, inglés, portugués.
struct Texts
{
	const char* faseInit;
	const char* network;
	const char* launch;
	const char* faseThreat;
	const char* threat;
	const char* detail;    // {0}: lo que se detectó
	const char* closing;   // {0}: segundos
	const char* faseBlocked;
	const char* blocked;
	const char* tooMany;
};

const Texts kTexts[] = {
	{ "Iniciando protecci\xC3\xB3n...", "Validando conexi\xC3\xB3n con el servidor...", "Abriendo main.exe",
	  "Se cerr\xC3\xB3 el juego", "AMENAZA DETECTADA", "Se detect\xC3\xB3 {0}. El cliente se va a cerrar.", "Cerrando en {0}...",
	  "No se abri\xC3\xB3 el juego", "ACCESO BLOQUEADO", "YA HAY UN JUEGO ABIERTO" },
	{ "Initializing protection...", "Validating server connection...", "Launching main.exe",
	  "The game was closed", "THREAT DETECTED", "{0} was detected. The client will close.", "Closing in {0}...",
	  "The game was not opened", "ACCESS BLOCKED", "A GAME IS ALREADY OPEN" },
	{ "Iniciando prote\xC3\xA7\xC3\xA3o...", "Validando conex\xC3\xA3o com o servidor...", "Abrindo main.exe",
	  "O jogo foi fechado", "AMEA\xC3\x87" "A DETECTADA", "{0} foi detectado. O cliente ser\xC3\xA1 fechado.", "Fechando em {0}...",
	  "O jogo n\xC3\xA3o foi aberto", "ACESSO BLOQUEADO", "J\xC3\x81 H\xC3\x81 UM JOGO ABERTO" },
};

std::string Fill(const char* text, const std::string& value)
{
	std::string s = text;
	const std::size_t at = s.find("{0}");
	if (at != std::string::npos) s.replace(at, 3, value);
	return s;
}

constexpr int kWidth = 720;
constexpr int kHeight = 458;
constexpr float kCorner = 8.0f;
constexpr float kSwapMs = 280.0f;   // .paso: salida y entrada del texto
constexpr float kPulseMs = 620.0f;  // .hilo.pulso

struct Stop
{
	float at;   // 0..1
	SDL_FColor color;
};

SDL_FColor Rgba(int r, int g, int b, float a = 1.0f)
{
	return SDL_FColor{ r / 255.0f, g / 255.0f, b / 255.0f, a };
}

// Rectángulo con un degradé horizontal.
void HorizontalGradient(SDL_Renderer* r, float x, float y, float w, float h, const std::vector<Stop>& stops)
{
	std::vector<SDL_Vertex> v;
	std::vector<int> index;
	for (std::size_t i = 0; i + 1 < stops.size(); ++i)
	{
		const float x0 = x + w * stops[i].at, x1 = x + w * stops[i + 1].at;
		const int base = static_cast<int>(v.size());
		v.push_back({ { x0, y }, stops[i].color, { 0, 0 } });
		v.push_back({ { x1, y }, stops[i + 1].color, { 0, 0 } });
		v.push_back({ { x1, y + h }, stops[i + 1].color, { 0, 0 } });
		v.push_back({ { x0, y + h }, stops[i].color, { 0, 0 } });
		for (int k : { 0, 1, 2, 0, 2, 3 }) index.push_back(base + k);
	}
	SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), index.data(), static_cast<int>(index.size()));
}

// Elipse con degradé radial (centro -> borde transparente).
void RadialEllipse(SDL_Renderer* r, float cx, float cy, float rx, float ry, SDL_FColor center, SDL_FColor middle)
{
	constexpr int kSegments = 40;
	std::vector<SDL_Vertex> v;
	std::vector<int> index;
	v.push_back({ { cx, cy }, center, { 0, 0 } });
	SDL_FColor edge = middle;
	edge.a = 0;
	for (int i = 0; i <= kSegments; ++i)
	{
		const float a = 2.0f * 3.14159265f * i / kSegments;
		v.push_back({ { cx + std::cos(a) * rx * 0.55f, cy + std::sin(a) * ry * 0.55f }, middle, { 0, 0 } });
		v.push_back({ { cx + std::cos(a) * rx, cy + std::sin(a) * ry }, edge, { 0, 0 } });
	}
	for (int i = 0; i < kSegments; ++i)
	{
		const int m0 = 1 + i * 2, e0 = m0 + 1, m1 = m0 + 2, e1 = m0 + 3;
		for (int k : { 0, m0, m1 }) index.push_back(k);
		for (int k : { m0, e0, e1, m0, e1, m1 }) index.push_back(k);
	}
	SDL_RenderGeometry(r, nullptr, v.data(), static_cast<int>(v.size()), index.data(), static_cast<int>(index.size()));
}

void Frame(SDL_Renderer* r, float inset, float width, SDL_FColor color)
{
	SDL_SetRenderDrawColorFloat(r, color.r, color.g, color.b, color.a);
	const SDL_FRect sides[] = {
		{ inset, inset, kWidth - 2 * inset, width },
		{ inset, kHeight - inset - width, kWidth - 2 * inset, width },
		{ inset, inset + width, width, kHeight - 2 * inset - 2 * width },
		{ kWidth - inset - width, inset + width, width, kHeight - 2 * inset - 2 * width },
	};
	SDL_RenderFillRects(r, sides, 4);
}

std::vector<unsigned char> ReadFile(const char* path)
{
	std::vector<unsigned char> data;
	FILE* f = std::fopen(path, "rb");
	if (!f) return data;
	unsigned char buffer[1 << 16];
	std::size_t n;
	while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) data.insert(data.end(), buffer, buffer + n);
	std::fclose(f);
	return data;
}

// Esquinas redondeadas.  En Windows una región sobre la ventana (como el
// launcher); en el resto, la forma de SDL.
#ifdef _WIN32
constexpr SDL_WindowFlags kShapeFlags = 0;

void RoundCorners(SDL_Window* window)
{
	HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!hwnd) return;
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);
	const int corner = static_cast<int>(2 * kCorner * SDL_GetWindowPixelDensity(window));
	HRGN region = CreateRoundRectRgn(0, 0, w + 1, h + 1, corner, corner);
	if (region && !SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);
}
#else
constexpr SDL_WindowFlags kShapeFlags = SDL_WINDOW_TRANSPARENT;

void RoundCorners(SDL_Window* window)
{
	SDL_Surface* mask = SDL_CreateSurface(kWidth, kHeight, SDL_PIXELFORMAT_RGBA32);
	if (!mask) return;
	for (int y = 0; y < kHeight; ++y)
	{
		Uint8* row = static_cast<Uint8*>(mask->pixels) + static_cast<std::size_t>(y) * static_cast<std::size_t>(mask->pitch);
		for (int x = 0; x < kWidth; ++x)
		{
			// Distancia a la esquina redondeada más cercana.
			const float dx = std::fmax(std::fmax(kCorner - (x + 0.5f), (x + 0.5f) - (kWidth - kCorner)), 0.0f);
			const float dy = std::fmax(std::fmax(kCorner - (y + 0.5f), (y + 0.5f) - (kHeight - kCorner)), 0.0f);
			const bool inside = dx * dx + dy * dy <= kCorner * kCorner;
			Uint8* p = row + x * 4;
			p[0] = p[1] = p[2] = 255;
			p[3] = inside ? 255 : 0;
		}
	}
	SDL_SetWindowShape(window, mask);
	SDL_DestroySurface(mask);
}
#endif

} // namespace

struct Banner::State
{
	SDL_Window* window = nullptr;
	SDL_Renderer* renderer = nullptr;
	SDL_Texture* background = nullptr;
	SDL_Texture* logo = nullptr;
	SDL_Texture* dragon = nullptr;
	Launcher::Font* title = nullptr;
	Launcher::Font* fase = nullptr;
	Launcher::Font* step = nullptr;
	std::vector<unsigned char> fontBold, fontItalic, fontRegular;
	int language = 0;
	Clock::time_point opened;
	Clock::time_point stepAt;
	std::string previous;   // texto que sale
	std::string current;    // texto que entra
	bool videoStarted = false;
	bool threat = false;    // AMENAZA: paleta roja
	const char* faseText = nullptr;   // texto de fase del estado rojo

	float Seconds() const
	{
		return std::chrono::duration<float>(Clock::now() - opened).count();
	}

	void Draw()
	{
		SDL_Renderer* r = renderer;
		SDL_SetRenderDrawColor(r, 0xEA, 0xF4, 0xFD, 255);
		SDL_RenderClear(r);
		SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
		if (background) SDL_RenderTexture(r, background, nullptr, nullptr);

		// Marco (.banner box-shadow): línea azul, hueco claro, línea azul tenue.
		if (threat)
		{
			// .banner.amenaza::after: tinte rojo desde la izquierda.
			RadialEllipse(r, kWidth * 0.2f, kHeight * 0.4f, kWidth * 1.1f, kHeight * 0.85f,
				Rgba(200, 50, 30, 0.20f), Rgba(200, 50, 30, 0.08f));
			Frame(r, 0, 2, Rgba(0xD9, 0x65, 0x4C));
			Frame(r, 2, 2, Rgba(255, 255, 255, 0.92f));
			Frame(r, 4, 1, Rgba(217, 101, 76, 0.55f));
		}
		else
		{
			Frame(r, 0, 2, Rgba(0x7C, 0xB8, 0xE4));
			Frame(r, 2, 2, Rgba(255, 255, 255, 0.92f));
			Frame(r, 4, 1, Rgba(124, 184, 228, 0.55f));
		}

		// Bloque izquierdo (.izq): logo centrado sobre el título, regla, fase.
		const float left = kWidth * 0.075f, top = kHeight * 0.07f;
		const float wTitle1 = title->Width("MU-"), wTitle = wTitle1 + title->Width("Guard");
		if (logo)
		{
			const SDL_FRect box{ left + (wTitle - 156) / 2, top, 156, 156 };
			SDL_RenderTexture(r, logo, nullptr, &box);
		}
		const float titleY = top + 156 + 4;
		title->Draw("MU-", left, titleY, threat ? SDL_Color{ 0xC8, 0x32, 0x1E, 255 } : SDL_Color{ 0x2F, 0x6E, 0xA8, 255 });
		title->Draw("Guard", left + wTitle1, titleY, threat ? SDL_Color{ 0xD9, 0x70, 0x5E, 255 } : SDL_Color{ 0x6F, 0x9F, 0xC8, 255 });
		const float ruleY = titleY + title->Height() * 1.2f + 9;
		if (threat)
			HorizontalGradient(r, left, ruleY, wTitle, 2,
				{ { 0, Rgba(0xFF, 0x5A, 0x3C) }, { 0.55f, Rgba(230, 120, 100, 0.45f) }, { 1, Rgba(230, 120, 100, 0) } });
		else
			HorizontalGradient(r, left, ruleY, wTitle, 2,
				{ { 0, Rgba(0x4A, 0xA3, 0xE8) }, { 0.55f, Rgba(120, 180, 230, 0.45f) }, { 1, Rgba(120, 180, 230, 0) } });
		fase->Draw(threat ? (this->faseText ? this->faseText : kTexts[language].faseThreat) : kTexts[language].faseInit, left, ruleY + 2 + 14,
			threat ? SDL_Color{ 0xB8, 0x44, 0x2F, 255 } : SDL_Color{ 0x4A, 0x7F, 0xB0, 255 });

		// Pie (.pie): el paso actual y el hilo con su destello.
		const float rowH = step->Height() * 1.3f;
		const float rowY = kHeight * 0.89f - rowH;
		const float since = std::chrono::duration<float, std::milli>(Clock::now() - stepAt).count();
		std::string text = current;
		float alpha = 1, shift = 0;
		if (since < kSwapMs && !previous.empty())
		{
			text = previous;
			alpha = 1 - since / kSwapMs;
			shift = -5 * since / kSwapMs;
		}
		else if (since < 2 * kSwapMs)
		{
			const float p = std::fmax(since - kSwapMs, 0.0f) / kSwapMs;
			alpha = p;
			shift = 5 * (1 - p);
		}
		step->Draw(text, left, rowY + shift, threat ? SDL_Color{ 0xC8, 0x32, 0x1E, static_cast<Uint8>(255 * alpha) }
		                                            : SDL_Color{ 0x5B, 0x8C, 0xBA, static_cast<Uint8>(255 * alpha) });

		const float lineX = left + step->Width(text) + 14, lineEnd = left + kWidth * 0.78f;
		const float lineY = rowY + rowH / 2;
		if (lineEnd > lineX)
		{
			if (threat)
				HorizontalGradient(r, lineX, lineY, lineEnd - lineX, 1,
					{ { 0, Rgba(210, 80, 60, 0.7f) }, { 0.7f, Rgba(210, 80, 60, 0.12f) }, { 1, Rgba(210, 80, 60, 0) } });
			else
				HorizontalGradient(r, lineX, lineY, lineEnd - lineX, 1,
					{ { 0, Rgba(90, 150, 200, 0.65f) }, { 0.7f, Rgba(90, 150, 200, 0.12f) }, { 1, Rgba(90, 150, 200, 0) } });
			const float pulse = (since - kSwapMs) / kPulseMs;
			if (pulse > 0 && pulse < 1)
			{
				const float a = pulse < 0.15f ? pulse / 0.15f : 1 - (pulse - 0.15f) / 0.85f;
				const float bx = lineX + 320 * pulse;
				if (bx < lineEnd)
					RadialEllipse(r, bx + 23, lineY + 0.5f, 23, 1.5f, Rgba(255, 255, 255, 0.95f * a), Rgba(120, 190, 240, 0.75f * a));
			}
		}

		// Mascota (.mascota) con su sombra en el piso, respirando cada 3,4 s.
		const float breathe = (1 - std::cos(Seconds() * 2 * 3.14159265f / 3.4f)) / 2;
		const float scale = 1 + 0.012f * breathe;
		const float dw = 232 * scale, dh = 229 * scale;
		// .banner.amenaza .mascota: se sacude tres veces (0,42 s cada una).
		const float sinceThreat = threat ? std::chrono::duration<float>(Clock::now() - stepAt).count() : 99;
		const float shake = sinceThreat < 1.26f ? 5 * std::sin(sinceThreat / 0.42f * 2 * 3.14159265f) : 0;
		const float dx = kWidth - kWidth * 0.03f - 232 - (dw - 232) / 2 + shake;
		const float dy = kHeight - kHeight * 0.08f - 229 - (dh - 229) / 2 - 5 * breathe;
		RadialEllipse(r, kWidth - kWidth * 0.03f - 116, kHeight - kHeight * 0.08f, 232 * 0.42f, 10,
			Rgba(45, 85, 130, 0.42f), Rgba(45, 85, 130, 0.16f));
		if (dragon)
		{
			const SDL_FRect box{ dx, dy, dw, dh };
			SDL_RenderTexture(r, dragon, nullptr, &box);
		}

		SDL_RenderPresent(r);
	}

	void Destroy()
	{
		delete title;
		delete fase;
		delete step;
		title = fase = step = nullptr;
		for (SDL_Texture* t : { background, logo, dragon })
			if (t) SDL_DestroyTexture(t);
		background = logo = dragon = nullptr;
		if (renderer) SDL_DestroyRenderer(renderer);
		if (window) SDL_DestroyWindow(window);
		renderer = nullptr;
		window = nullptr;
		if (videoStarted) SDL_QuitSubSystem(SDL_INIT_VIDEO);
		videoStarted = false;
	}
};

Banner::Banner() : m(new State) {}

Banner::~Banner()
{
	this->Close();
	delete m;
}

bool Banner::Open(int language)
{
	m->language = (language >= 0 && language < 3) ? language : 0;
	m->fontBold = ReadFile("System/Font/LiberationSans-Bold.ttf");
	m->fontItalic = ReadFile("System/Font/LiberationSans-Italic.ttf");
	m->fontRegular = ReadFile("System/Font/LiberationSans-Regular.ttf");
	if (m->fontBold.empty() || m->fontItalic.empty() || m->fontRegular.empty()) return false;

	if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
	m->videoStarted = true;
	m->window = SDL_CreateWindow("MU-Guard", kWidth, kHeight,
		SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN | kShapeFlags);
	m->renderer = m->window ? SDL_CreateRenderer(m->window, nullptr) : nullptr;
	if (!m->renderer)
	{
		m->Destroy();
		return false;
	}
	SDL_SetRenderLogicalPresentation(m->renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetRenderVSync(m->renderer, 1);
	const float scale = SDL_GetWindowPixelDensity(m->window);

	m->background = Launcher::LoadPng(m->renderer, LauncherRes::GuardBackground, LauncherRes::GuardBackgroundSize);
	m->logo = Launcher::LoadPng(m->renderer, LauncherRes::GuardLogo, LauncherRes::GuardLogoSize);
	m->dragon = Launcher::LoadPng(m->renderer, LauncherRes::GuardDragon, LauncherRes::GuardDragonSize);
	m->title = new Launcher::Font(m->renderer, m->fontBold.data(), m->fontBold.size(), 26, scale);
	m->fase = new Launcher::Font(m->renderer, m->fontItalic.data(), m->fontItalic.size(), 15, scale);
	m->step = new Launcher::Font(m->renderer, m->fontRegular.data(), m->fontRegular.size(), 14, scale);
	if (!m->title->Ok() || !m->fase->Ok() || !m->step->Ok())
	{
		m->Destroy();
		return false;
	}

	RoundCorners(m->window);
	m->opened = m->stepAt = Clock::now();
	m->Draw();
	SDL_ShowWindow(m->window);
	SDL_RaiseWindow(m->window);
	return true;
}

bool Banner::IsOpen() const
{
	return m->window != nullptr;
}

void Banner::Step(BannerStep step)
{
	m->previous = m->current;
	m->current = step == BannerStep::Network ? kTexts[m->language].network : kTexts[m->language].launch;
	m->stepAt = Clock::now();
}

bool Banner::Wait(const std::function<bool()>& done, int minMs)
{
	if (!this->IsOpen())
	{
		while (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return true;
	}
	for (;;)
	{
		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) return false;
		}
		m->Draw();
		const auto shown = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - m->stepAt).count();
		if (shown >= minMs && done()) return true;
		SDL_Delay(5);   // con VSync el Present ya espera
	}
}

void Banner::Close()
{
	m->Destroy();
}

void Banner::Threat(int language, const std::string& what, const std::string& notice)
{
	const int lang = (language >= 0 && language < 3) ? language : 0;
	const Texts& t = kTexts[lang];
	std::vector<std::string> lines{ Fill(t.detail, what) };
	if (!notice.empty()) lines.push_back(notice);
	// El aviso sigue a la vista con la cuenta regresiva: si no, dura 2 s.
	this->Alert(lang, t.faseThreat, t.threat, lines, !notice.empty());
}

void Banner::Blocked(int language, const std::string& detail)
{
	const int lang = (language >= 0 && language < 3) ? language : 0;
	const Texts& t = kTexts[lang];
	this->Alert(lang, t.faseBlocked, t.blocked, { detail }, true);
}

void Banner::Refused(int language, const std::string& detail)
{
	const int lang = (language >= 0 && language < 3) ? language : 0;
	const Texts& t = kTexts[lang];
	this->Alert(lang, t.faseBlocked, t.tooMany, { detail }, true);
}

void Banner::Alert(int language, const char* faseText, const char* headline, const std::vector<std::string>& lines, bool keepLast)
{
	const Texts& t = kTexts[language];
	if (!this->IsOpen() && !this->Open(language))
	{
		std::string text = headline;
		for (const std::string& line : lines) text += "\n\n" + line;
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "MU-Guard", text.c_str(), nullptr);
		return;
	}
	m->threat = true;
	m->faseText = faseText;
	m->current.clear();
	this->Step(BannerStep::Network);   // sólo para la transición; el texto se pisa abajo
	m->current = headline;
	SDL_RaiseWindow(m->window);
	// Como amenazaDetectada() del template: el aviso, el detalle a los 1,1 s y
	// la cuenta regresiva desde los 2,6 s.  Cada línea extra, 2 s más.
	auto show = [&](const std::string& text, int ms) {
		m->previous = m->current;
		m->current = text;
		m->stepAt = Clock::now();
		this->Wait([]() { return true; }, ms);
	};
	this->Wait([]() { return true; }, 1100);
	for (std::size_t i = 0; i < lines.size(); ++i) show(lines[i], i == 0 ? 1500 : 2500);
	const std::string before = (keepLast && !lines.empty()) ? lines.back() + "  " : std::string();
	for (int left = 5; left > 0; --left) show(before + Fill(t.closing, std::to_string(left)), 1000);
}

#else // sin MU_GUARD_BANNER: no hay ventana, Wait() sólo espera.

struct Banner::State {};

Banner::Banner() : m(new State) {}
Banner::~Banner() { delete m; }
bool Banner::Open(int) { return false; }
bool Banner::IsOpen() const { return false; }
void Banner::Step(BannerStep) {}
void Banner::Close() {}
void Banner::Threat(int, const std::string&, const std::string&) {}
void Banner::Blocked(int, const std::string&) {}
void Banner::Refused(int, const std::string&) {}
void Banner::Alert(int, const char*, const char*, const std::vector<std::string>&, bool) {}

bool Banner::Wait(const std::function<bool()>& done, int)
{
	while (!done()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return true;
}

#endif

} // namespace Guard
