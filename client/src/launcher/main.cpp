// Launcher del cliente (Fase 5).  Ventana con uno de los temas de kayito
// (ThemeSpec.h): actualiza el cliente al abrir, "Opciones" edita
// User/Config.ini y User/launcher.toml, y "Jugar" arranca el cliente.
//
// Uso: launcher [--root=<carpeta del cliente>] [--options[=<pestaña>]]
// (--updated y --rolled-back los pone el propio launcher al reiniciarse)
// Sin --root, la carpeta del cliente es la del launcher (o la de arriba si el
// launcher está en Launcher/, donde lo pone el overlay).  --options abre directo
// la pantalla de opciones (0 = General ... 7 = Descargas).

#include "Gfx.h"
#include "LauncherConfig.h"
#include "OptionsModel.h"
#include "SelfUpdate.h"
#include "Resources.h"
#include "ThemeSpec.h"
#include "UpdateFlow.h"
#include "WebPanel.h"

#include "platform/RunningLock.h"
#include "update/FileUtil.h"
#include "update/HttpTransport.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

using namespace Launcher;

namespace {

// El tema compilado: medidas de la ventana y de todo lo que se dibuja.
const ThemeSpec g_Theme = MakeTheme();

const SDL_Color kWhite{ 255, 255, 255, 255 };

bool Inside(const SDL_FRect& r, float x, float y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

struct Button
{
	SDL_FRect rect;
	const char* label;
	const ButtonStyle* style = nullptr;   // nullptr: el de los botones del tema
	bool enabled = true;
	bool hover = false;
	bool pressed = false;
};

// Unidades de ventana por unidad lógica (la ventana se agranda con "Tamaño del
// launcher"; el dibujo sigue en el tamaño del tema).
float g_WindowScale = 1.0f;
// La escala elegida y si se ven las opciones (en algunos temas la ventana
// toma otro tamaño mientras tanto).
float g_Scale = 1.0f;
bool g_OptionsMode = false;

int LogicalW() { return g_OptionsMode ? g_Theme.OptionsWidth() : g_Theme.width; }
int LogicalH() { return g_OptionsMode ? g_Theme.OptionsHeight() : g_Theme.height; }

SDL_FRect DragRect()
{
	return g_OptionsMode && g_Theme.optionsDrag.w > 0 ? g_Theme.optionsDrag : g_Theme.drag;
}

SDL_HitTestResult HitTest(SDL_Window*, const SDL_Point* p, void*)
{
	const SDL_FRect d = DragRect();
	const float x = static_cast<float>(p->x) / g_WindowScale, y = static_cast<float>(p->y) / g_WindowScale;
	return x >= d.x && x < d.x + d.w && y >= d.y && y < d.y + d.h ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

// Escala de la ventana: la elegida (percent) o la de la pantalla (0), sin pasar
// del área útil de la pantalla.
float ChooseScale(SDL_Window* window, int percent)
{
	const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
	float s = percent > 0 ? static_cast<float>(percent) / 100.0f : SDL_GetDisplayContentScale(display);
	if (!(s >= 1.0f)) s = 1.0f;
	SDL_Rect area;
	if (SDL_GetDisplayUsableBounds(display, &area))
	{
		// Que entren las dos pantallas (principal y opciones).
		const float w = static_cast<float>(std::max(g_Theme.width, g_Theme.OptionsWidth()));
		const float h = static_cast<float>(std::max(g_Theme.height, g_Theme.OptionsHeight()));
		const float fit = std::min(static_cast<float>(area.w) / w, static_cast<float>(area.h) / h) * 0.95f;
		if (s > fit) s = std::max(1.0f, fit);
	}
	return s;
}

// Ajusta la ventana a la pantalla actual (principal u opciones) y a g_Scale:
// centrada en la pantalla, o en el mismo centro de antes.  Devuelve los píxeles
// por unidad lógica, para rasterizar las fuentes a ese tamaño.
float FitWindow(SDL_Window* window, SDL_Renderer* renderer, bool centerOnDisplay)
{
	const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
	const int w = LogicalW(), h = LogicalH();
	int x = 0, y = 0, oldW = 0, oldH = 0;
	SDL_GetWindowPosition(window, &x, &y);
	SDL_GetWindowSize(window, &oldW, &oldH);
	const int newW = static_cast<int>(std::lround(w * g_Scale)), newH = static_cast<int>(std::lround(h * g_Scale));
	SDL_SetWindowSize(window, newW, newH);
	if (centerOnDisplay)
		SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
	else
		SDL_SetWindowPosition(window, x + (oldW - newW) / 2, y + (oldH - newH) / 2);
	SDL_SetRenderLogicalPresentation(renderer, w, h, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SyncWindow(window);
	int ww = w, wh = h, ow = w, oh = h;
	SDL_GetWindowSize(window, &ww, &wh);
	SDL_GetRenderOutputSize(renderer, &ow, &oh);
	g_WindowScale = std::min(static_cast<float>(ww) / w, static_cast<float>(wh) / h);
	const float px = std::min(static_cast<float>(ow) / w, static_cast<float>(oh) / h);
	return px > 1.0f ? px : 1.0f;
}

// Forma de la ventana (temas con ThemeSpec::shaped): lo transparente de la
// imagen no se dibuja ni recibe clics.  shape nullptr: la ventana entera.
//
// En Windows es una región (SetWindowRgn) sobre una ventana normal: con
// SDL_WINDOW_TRANSPARENT, que es lo que pide SDL_SetWindowShape, WebView2 no
// dibuja el panel web.  La región es binaria, igual que el TransparencyKey del
// original.  En el resto de los sistemas va por SDL.
#ifdef _WIN32
constexpr SDL_WindowFlags kShapedFlags = 0;

void ApplyShape(SDL_Window* window, SDL_Surface* shape)
{
	HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!hwnd) return;
	if (!shape)
	{
		SetWindowRgn(hwnd, nullptr, TRUE);
		return;
	}
	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(window, &w, &h);
	SDL_Surface* s = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
	if (!s) return;
	if (!SDL_StretchSurface(shape, nullptr, s, nullptr, SDL_SCALEMODE_LINEAR))
	{
		SDL_DestroySurface(s);
		return;
	}
	// Una tira por fila con cada tramo opaco.
	HRGN region = CreateRectRgn(0, 0, 0, 0);
	const Uint8* px = static_cast<const Uint8*>(s->pixels);
	for (int y = 0; y < h; ++y)
	{
		const Uint8* row = px + static_cast<std::size_t>(y) * static_cast<std::size_t>(s->pitch);
		int start = -1;
		for (int x = 0; x <= w; ++x)
		{
			const bool opaque = x < w && row[x * 4 + 3] >= 128;
			if (opaque && start < 0) start = x;
			else if (!opaque && start >= 0)
			{
				HRGN span = CreateRectRgn(start, y, x, y + 1);
				CombineRgn(region, region, span, RGN_OR);
				DeleteObject(span);
				start = -1;
			}
		}
	}
	SDL_DestroySurface(s);
	if (!SetWindowRgn(hwnd, region, TRUE)) DeleteObject(region);   // si sale bien, la región es de Windows
}
#else
constexpr SDL_WindowFlags kShapedFlags = SDL_WINDOW_TRANSPARENT;

void ApplyShape(SDL_Window* window, SDL_Surface* shape)
{
	// SDL no acepta nullptr: la ventana entera es una forma opaca de 1x1.
	SDL_Surface* full = nullptr;
	if (!shape)
	{
		full = SDL_CreateSurface(1, 1, SDL_PIXELFORMAT_RGBA32);
		if (!full) return;
		SDL_FillSurfaceRect(full, nullptr, SDL_MapSurfaceRGBA(full, 255, 255, 255, 255));
	}
	SDL_SetWindowShape(window, shape ? shape : full);   // SDL la vuelve a aplicar al cambiar el tamaño
	if (full) SDL_DestroySurface(full);
}
#endif

float ApplyScale(SDL_Window* window, SDL_Renderer* renderer, int percent)
{
	g_Scale = ChooseScale(window, percent);
	return FitWindow(window, renderer, true);
}

std::string Utf8(const std::filesystem::path& p)
{
	const auto s = p.generic_u8string();
	return std::string(s.begin(), s.end());
}

// Ruta del ejecutable del launcher, con "/".
std::string ExecutablePath()
{
#ifdef _WIN32
	std::wstring buf(32768, L'\0');
	const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
	buf.resize(n);
	return Utf8(std::filesystem::path(buf));
#else
	std::error_code ec;
	return Utf8(std::filesystem::read_symlink("/proc/self/exe", ec));
#endif
}

// La carpeta del cliente: --root, o la del launcher (la de arriba si el
// launcher está en su lugar de siempre, Launcher/).
std::string RootFor(int argc, char** argv, const std::string& exe)
{
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--root=", 7) == 0) return argv[i] + 7;
	std::filesystem::path dir = std::filesystem::u8path(exe).parent_path();
	std::string name = Utf8(dir.filename());
	for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	if (name == "launcher") dir = dir.parent_path();
	const std::string s = Utf8(dir);
	return s.empty() ? std::string(".") : s;
}

// El launcher relativo a la raíz ("Launcher/launcher.exe"), o vacío si está
// fuera de la carpeta del cliente.
std::string SelfPathIn(const std::string& root, const std::string& exe)
{
	std::error_code ec;
	const std::string r = Utf8(std::filesystem::relative(std::filesystem::u8path(exe), std::filesystem::u8path(root), ec));
	if (ec || r.empty() || r.compare(0, 2, "..") == 0 || !Update::IsSafeRelativePath(r)) return std::string();
	return r;
}

bool LaunchProgram(const std::string& path, const std::vector<std::string>& extraArgs, const std::string* clientRoot,
                   std::string* error)
{
	std::vector<const char*> args{ path.c_str() };
	for (const std::string& a : extraArgs) args.push_back(a.c_str());
	args.push_back(nullptr);
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args.data()));
	SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
	SDL_Environment* env = nullptr;
#ifndef _WIN32
	::chmod(path.c_str(), 0755);
	if (clientRoot)
	{
		// El cliente de Linux toma sus carpetas de acá (ver platform/UserPath.h).
		env = SDL_CreateEnvironment(true);
		SDL_SetEnvironmentVariable(env, "MU_CLIENT_ROOT", clientRoot->c_str(), true);
		SDL_SetEnvironmentVariable(env, "MU_ASSET_ROOT", clientRoot->c_str(), true);
		SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
	}
#else
	(void)clientRoot;
#endif
	SDL_Process* proc = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (env) SDL_DestroyEnvironment(env);
	if (!proc)
	{
		if (error) *error = std::string("No se pudo abrir ") + path + ": " + SDL_GetError();
		return false;
	}
	SDL_DestroyProcess(proc);   // no lo cierra: sólo suelta el objeto
	return true;
}

bool LaunchGame(const std::string& root, const std::string& exe, std::string* error)
{
	const std::string path = Update::JoinPath(root, exe);
	if (!Update::FileExists(path))
	{
		*error = "No está " + exe + ".";
		return false;
	}
	return LaunchProgram(path, {}, &root, error);
}

// Corta el texto en líneas que entren en `width` (respeta los saltos de línea).
std::vector<std::string> Wrap(const Font& font, const std::string& text, float width)
{
	std::vector<std::string> lines;
	std::size_t start = 0;
	while (start <= text.size())
	{
		std::size_t nl = text.find('\n', start);
		if (nl == std::string::npos) nl = text.size();
		const std::string para = text.substr(start, nl - start);
		std::string line;
		std::size_t i = 0;
		while (i <= para.size())
		{
			std::size_t sp = para.find(' ', i);
			if (sp == std::string::npos) sp = para.size();
			const std::string word = para.substr(i, sp - i);
			const std::string candidate = line.empty() ? word : line + " " + word;
			if (!line.empty() && font.Width(candidate) > width)
			{
				lines.push_back(line);
				line = word;
			}
			else
				line = candidate;
			i = sp + 1;
		}
		lines.push_back(font.Fit(line, width));
		start = nl + 1;
	}
	return lines;
}

void DrawBevel(SDL_Renderer* r, const SDL_FRect& box, bool sunken = true)
{
	// Borde 3D, como el Fixed3D del original.
	const Uint8 a = sunken ? 128 : 255, b = sunken ? 255 : 128;
	SDL_SetRenderDrawColor(r, a, a, a, 255);
	SDL_RenderLine(r, box.x, box.y, box.x + box.w - 1, box.y);
	SDL_RenderLine(r, box.x, box.y, box.x, box.y + box.h - 1);
	SDL_SetRenderDrawColor(r, b, b, b, 255);
	SDL_RenderLine(r, box.x + box.w - 1, box.y, box.x + box.w - 1, box.y + box.h - 1);
	SDL_RenderLine(r, box.x, box.y + box.h - 1, box.x + box.w - 1, box.y + box.h - 1);
}

void Fill(SDL_Renderer* r, const SDL_FRect& box, SDL_Color c)
{
	SDL_SetRenderDrawBlendMode(r, c.a == 255 ? SDL_BLENDMODE_NONE : SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
	SDL_RenderFillRect(r, &box);
}

struct Ui
{
	SDL_Renderer* r = nullptr;
	std::map<std::string, SDL_Texture*> images;   // las del tema, por nombre
	std::unique_ptr<Font> title, label, regular, tiny, notice;
	std::map<float, std::unique_ptr<Font>> sized;   // la de los botones con fontSize
	float fontScale = 1.0f;

	// La imagen del tema con ese nombre; nullptr si el nombre está vacío o el
	// tema no la trae.
	SDL_Texture* Image(const std::string& name)
	{
		if (name.empty()) return nullptr;
		auto it = images.find(name);
		if (it != images.end()) return it->second;
		const unsigned char* data = nullptr;
		std::size_t size = 0;
		SDL_Texture* t = LauncherRes::FindImage(name, &data, &size) ? LoadPng(r, data, size) : nullptr;
		images[name] = t;
		return t;
	}

	void MakeFonts(float scale)
	{
		using namespace LauncherRes;
		title = std::make_unique<Font>(r, FontBold, FontBoldSize, 13.0f, scale);
		label = std::make_unique<Font>(r, FontBold, FontBoldSize, 12.5f, scale);
		regular = std::make_unique<Font>(r, FontRegular, FontRegularSize, 12.5f, scale);
		tiny = std::make_unique<Font>(r, FontBold, FontBoldSize, 10.0f, scale);
		notice = std::make_unique<Font>(r, FontRegular, FontRegularSize, 11.0f, scale);
		sized.clear();
		fontScale = scale;
	}

	Font& Bold(float size)
	{
		auto& f = sized[size];
		if (!f) f = std::make_unique<Font>(r, LauncherRes::FontBold, LauncherRes::FontBoldSize, size, fontScale);
		return *f;
	}

	void DrawImage(const std::string& name, const SDL_FRect* where)
	{
		if (SDL_Texture* t = Image(name)) SDL_RenderTexture(r, t, nullptr, where);
	}

	void DrawButton(const Button& b)
	{
		const ButtonStyle& s = b.style ? *b.style : g_Theme.buttonStyle;
		SDL_Texture* t = Image(!b.enabled ? s.disabled : (b.hover ? s.hover : s.normal));
		if (!t) t = Image(s.normal);
		if (t) SDL_RenderTexture(r, t, nullptr, &b.rect);
		if (!b.label || !*b.label) return;
		Font& f = s.fontSize > 0 ? Bold(s.fontSize) : *label;
		SDL_FRect box = b.rect;
		if (s.textTop) box.h = f.Height() + 6;
		if (b.pressed) box.y += 1;
		f.DrawCentered(b.label, box, b.enabled ? s.text : s.textDisabled);
	}

	// Barra de progreso: color o imagen recortada a frac.
	void DrawBar(const ProgressBarSpec& bs, float frac, bool failed)
	{
		if (bs.rect.w <= 0) return;
		if (bs.bevel) DrawBevel(r, bs.rect);
		const float bb = bs.bevel ? 2.0f : 0.0f;
		const SDL_FRect track{ bs.rect.x + bb, bs.rect.y + bb, bs.rect.w - 2 * bb, bs.rect.h - 2 * bb };
		if (bs.track.a) Fill(r, track, bs.track);
		frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
		if (frac > 0)
		{
			const SDL_FRect fill{ track.x, track.y, track.w * frac, track.h };
			SDL_Texture* img = failed ? nullptr : Image(bs.image);
			if (img)
			{
				float iw = 0, ih = 0;
				SDL_GetTextureSize(img, &iw, &ih);
				const SDL_FRect src{ 0, 0, iw * frac, ih };
				SDL_RenderTexture(r, img, &src, &fill);
			}
			else
				Fill(r, fill, failed ? bs.failed : bs.fill);
		}
		if (bs.percentPos.x > 0)
			tiny->Draw(std::to_string(static_cast<int>(frac * 100.0f + 0.5f)) + "%", bs.percentPos.x, bs.percentPos.y, bs.percentColor);
	}

	~Ui()
	{
		for (auto& i : images)
			if (i.second) SDL_DestroyTexture(i.second);
	}
};

// Pantalla de opciones: pestañas en dos filas y una fila por opción con
// flechas para cambiar el valor.
struct OptionsScreen
{
	OptionsEditor editor;
	std::size_t tab = 0;
	int editing = -1;         // fila de texto que se está escribiendo (de la pestaña actual)
	std::string message;
	Button save{ g_Theme.save, "Guardar" };
	Button back{ g_Theme.back, "Volver" };
	Button verify{ { 0, 0, 150, 26 }, "Verificar archivos" };
	Button web{ { 0, 0, 150, 26 }, "Ir a la web" };

	// Todo relativo al panel del tema (en el V1: 9,33, 433 de ancho y cuatro
	// pestañas por fila).
	static constexpr float kTabY = 3, kTabH = 20, kRowH = 23;
	const SDL_FRect& P() const { return g_Theme.optionsPanel; }
	std::size_t PerRow() const { return static_cast<std::size_t>(std::max(1, g_Theme.tabsPerRow)); }
	float TabW() const { return std::round((P().w - 6) / static_cast<float>(PerRow()) - 1); }
	float RowY() const
	{
		const std::size_t rows = (editor.Tabs().size() + PerRow() - 1) / PerRow();
		return kTabY + static_cast<float>(rows) * (kTabH + 2) + 6;
	}

	SDL_FRect TabRect(std::size_t i) const
	{
		return { P().x + 3 + static_cast<float>(i % PerRow()) * (TabW() + 1), P().y + kTabY + static_cast<float>(i / PerRow()) * (kTabH + 2),
		         TabW(), kTabH };
	}
	SDL_FRect RowRect(std::size_t i) const { return { P().x + 5, P().y + RowY() + static_cast<float>(i) * kRowH, P().w - 11, kRowH - 2 }; }
	float ValueX() const { return P().x + P().w - 180; }
	SDL_FRect LeftArrow(std::size_t i) const { const SDL_FRect r = RowRect(i); return { ValueX(), r.y, 20, r.h }; }
	SDL_FRect RightArrow(std::size_t i) const { const SDL_FRect r = RowRect(i); return { ValueX() + 150, r.y, 20, r.h }; }
	SDL_FRect TextBox(std::size_t i) const { const SDL_FRect r = RowRect(i); return { ValueX(), r.y + 1, 170, r.h - 2 }; }
	float BelowRows(std::size_t rows) const { return P().y + RowY() + static_cast<float>(rows) * kRowH + 10; }
	bool IsDownloads() const { return editor.Tabs()[tab].name == "Descargas"; }
	bool IsGeneral() const { return editor.Tabs()[tab].name == "General"; }
};

} // namespace

int main(int argc, char** argv)
{
	const std::string exe = ExecutablePath();
	std::string root = RootFor(argc, argv, exe);
	std::error_code ec;
	std::filesystem::current_path(std::filesystem::u8path(root), ec);
	const std::string userDir = UserDirFor(root);
	const std::string selfPath = SelfPathIn(root, exe);
	std::vector<std::string> passArgs;   // los que se le pasan al reiniciarse
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--root=", 7) == 0) passArgs.push_back(argv[i]);

	// Lo primero: si el arranque anterior fue el de una versión nueva que no
	// llegó a confirmarse, se vuelve a la anterior (SelfUpdate.h).
	const SelfPaths self = SelfPathsFor(exe, Update::JoinPath(userDir, "Cache"));
	StartupCheck startup = StartupCheck::Normal;
	if (!selfPath.empty())
	{
		startup = CheckAtStartup(self);
		if (startup == StartupCheck::RolledBack)
		{
			std::vector<std::string> a = passArgs;
			a.push_back("--rolled-back");
			return LaunchProgram(exe, a, nullptr, nullptr) ? 0 : 1;
		}
	}
	bool rolledBack = false;
	for (int i = 1; i < argc; ++i)
		if (std::strcmp(argv[i], "--rolled-back") == 0) rolledBack = true;
	const std::string tomlPath = Update::JoinPath(userDir, "launcher.toml");
	const std::string iniPath = Update::JoinPath(userDir, "Config.ini");
	// En Windows la plantilla está en la raíz y el cliente la copia a User/ al
	// arrancar (platform/UserPath.h); hasta entonces se parte de ella.
	const std::string iniTemplate = Update::JoinPath(root, "Config.ini");

	auto loadConfig = [&](std::vector<std::string>* warnings) {
		Config c = CompiledConfig();
		ApplyLauncherToml(tomlPath, c, warnings);
		// Sin HD en launcher.toml manda la opción del juego (las texturas HD
		// prendidas se bajan).
		if (!c.hdSet) c.hd = HDTexturesEnabled(Update::FileExists(iniPath) ? iniPath : iniTemplate);
		return c;
	};
	std::vector<std::string> warnings;
	Config config = loadConfig(&warnings);
	config.selfPath = selfPath;
	if (rolledBack) warnings.push_back("La versión nueva del launcher no arrancó; se volvió a la anterior.");
	if (startup == StartupCheck::RollbackFailed) warnings.push_back("La versión nueva del launcher no arrancó y no se pudo volver a la anterior.");

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launcher", SDL_GetError(), nullptr);
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow(config.title.c_str(), g_Theme.width, g_Theme.height,
	                                      SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN |
	                                          (g_Theme.shaped ? kShapedFlags : 0));
	SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
	if (!renderer)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launcher", SDL_GetError(), nullptr);
		SDL_Quit();
		return 1;
	}
	SDL_SetRenderLogicalPresentation(renderer, g_Theme.width, g_Theme.height, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetRenderVSync(renderer, 1);
	SDL_SetWindowHitTest(window, HitTest, nullptr);
	// Forma de la ventana: la del fondo en la principal, entera en las opciones.
	SDL_Surface* shapeMain = nullptr;
	if (g_Theme.shaped)
	{
		const unsigned char* data = nullptr;
		std::size_t size = 0;
		if (LauncherRes::FindImage(g_Theme.background, &data, &size)) shapeMain = LoadPngSurface(data, size);
	}
	auto reshape = [&] {
		if (shapeMain) ApplyShape(window, g_OptionsMode ? nullptr : shapeMain);
	};
	int appliedScale = config.scale;
	float fontScale = ApplyScale(window, renderer, appliedScale);
	reshape();
	SDL_ShowWindow(window);

	{
		Ui ui;
		ui.r = renderer;
		ui.MakeFonts(fontScale);
		// La web del server en el panel de la imagen (sólo Windows, WebPanel.h).
		WebPanel webPanel(window, Update::JoinPath(Update::JoinPath(userDir, "Cache"), "WebView2"));
		std::string webShownUrl;

		Button options{ g_Theme.options, "Opciones" };
		Button play{ g_Theme.play, "Jugar", &g_Theme.PlayStyle() };
		Button close{ g_Theme.close, "Cerrar" };
		Button exitX{ g_Theme.exit, "", &g_Theme.exitStyle };
		// Principal u opciones: en algunos temas la ventana cambia de tamaño.
		auto setMode = [&](bool optionsMode) {
			if (g_OptionsMode == optionsMode) return;
			g_OptionsMode = optionsMode;
			if (g_Theme.OptionsWidth() == g_Theme.width && g_Theme.OptionsHeight() == g_Theme.height) return;
			FitWindow(window, renderer, false);
			reshape();
		};

		auto startFlow = [&](const Config& c) {
			auto f = std::make_unique<UpdateFlow>(c, root, std::make_unique<AnyTransport>(std::make_unique<Update::HttpTransport>()));
			f->Start();
			return f;
		};
		std::unique_ptr<UpdateFlow> flow = startFlow(config);
		std::string launchError;
		bool launched = false;   // se arrancó el juego con descargas en curso: el launcher sigue
		Button pause{ g_Theme.pause, "Pausar" };
		const std::string lockPath = Update::JoinPath(Update::JoinPath(userDir, "Cache"), "running.lock");

		std::unique_ptr<OptionsScreen> opts;
		auto stopEditing = [&] {
			if (opts && opts->editing >= 0) SDL_StopTextInput(window);
			if (opts) opts->editing = -1;
		};
		auto startEditing = [&](int row) {
			if (opts->editing == row) return;
			stopEditing();
			opts->editing = row;
			const SDL_FRect box = opts->TextBox(static_cast<std::size_t>(row));
			const SDL_Rect area{ static_cast<int>(box.x * g_WindowScale), static_cast<int>(box.y * g_WindowScale),
			                     static_cast<int>(box.w * g_WindowScale), static_cast<int>(box.h * g_WindowScale) };
			SDL_SetTextInputArea(window, &area, 0);
			SDL_StartTextInput(window);
		};
		auto closeOptions = [&] {
			stopEditing();
			opts.reset();
			setMode(false);
		};
		auto openOptions = [&](std::size_t tab) {
			opts = std::make_unique<OptionsScreen>();
			setMode(true);
			if (!opts->editor.Load(iniPath, iniTemplate, tomlPath)) opts->message = "No se pudo leer Config.ini.";
			else if (RunningLock::IsHeld(lockPath))
			{
				// El juego también escribe Config.ini desde su menú.
				opts->editor.SetGameLocked(true);
				opts->message = "El juego está abierto: sus opciones se ven pero se cambian al cerrarlo.";
			}
			if (tab < opts->editor.Tabs().size()) opts->tab = tab;
		};
		for (int i = 1; i < argc; ++i)
			if (std::strncmp(argv[i], "--options", 9) == 0)
				openOptions(argv[i][9] == '=' ? static_cast<std::size_t>(std::atoi(argv[i] + 10)) : 0);
		const Uint64 started = SDL_GetTicks();
		bool confirmed = startup != StartupCheck::Confirming;
		bool restartTried = false;
		bool running = true;
		while (running)
		{
			const Status st = flow->Snapshot();
			// Versión nueva andando hace 3 s: se confirma y se borra la anterior.
			if (!confirmed && SDL_GetTicks() - started > 3000)
			{
				ConfirmHealthy(self);
				confirmed = true;
			}
			// Se instaló un launcher nuevo: arrancarlo y salir.
			if (st.phase == Phase::Restart && !restartTried)
			{
				restartTried = true;
				std::vector<std::string> a = passArgs;
				a.push_back("--updated");
				if (LaunchProgram(exe, a, nullptr, &launchError)) break;
			}
			play.enabled = st.canPlay;
			// Jugando con lo que quedaba por bajar: al terminar, el launcher se cierra.
			if (launched && flow->Finished() && st.phase == Phase::Ready) running = false;
			const bool canPause = st.background && !flow->Finished() && st.waitingForClose == 0;
			pause.label = st.paused ? "Seguir" : "Pausar";
			Button* mainButtons[] = { &options, &play, &close, &exitX };
			std::vector<Button*> buttons;
			if (opts)
			{
				opts->save.enabled = opts->editor.Dirty();
				opts->verify.enabled = flow->Finished();
				buttons = { &opts->save, &opts->back };
				if (opts->IsDownloads()) buttons.push_back(&opts->verify);
				opts->web.enabled = !st.webUrl.empty();
				if (opts->IsGeneral()) buttons.push_back(&opts->web);
			}
			else
			{
				buttons.assign(std::begin(mainButtons), std::end(mainButtons));
				if (exitX.rect.w <= 0) buttons.pop_back();
				if (canPause) buttons.push_back(&pause);
			}

			SDL_Event e;
			while (SDL_PollEvent(&e))
			{
				if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
				if (opts && opts->editing >= 0)
				{
					const std::size_t row = static_cast<std::size_t>(opts->editing);
					if (e.type == SDL_EVENT_TEXT_INPUT)
					{
						opts->editor.SetText(opts->tab, row, opts->editor.Text(opts->tab, row) + e.text.text);
						continue;
					}
					if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_BACKSPACE)
					{
						std::string t = opts->editor.Text(opts->tab, row);
						if (!t.empty()) t.pop_back();
						opts->editor.SetText(opts->tab, row, t);
						continue;
					}
					if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_RETURN || e.key.key == SDLK_KP_ENTER ||
					                                     e.key.key == SDLK_ESCAPE || e.key.key == SDLK_TAB))
					{
						stopEditing();
						continue;
					}
				}
				if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)
				{
					if (opts) closeOptions();
					else running = false;
					continue;
				}
				if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
					SDL_ConvertEventToRenderCoordinates(renderer, &e);
				if (e.type == SDL_EVENT_MOUSE_MOTION)
					for (Button* b : buttons) b->hover = Inside(b->rect, e.motion.x, e.motion.y);
				if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
				{
					for (Button* b : buttons) b->pressed = b->enabled && Inside(b->rect, e.button.x, e.button.y);
					if (opts)
					{
						const float x = e.button.x, y = e.button.y;
						int textRow = -1;
						for (std::size_t i = 0; i < opts->editor.Tabs().size(); ++i)
							if (Inside(opts->TabRect(i), x, y) && i != opts->tab)
							{
								stopEditing();
								opts->tab = i;
							}
						const auto& rows = opts->editor.Tabs()[opts->tab].rows;
						for (std::size_t i = 0; i < rows.size(); ++i)
						{
							if (rows[i].text)
							{
								if (Inside(opts->RowRect(i), x, y) && opts->editor.Editable(opts->tab, i)) textRow = static_cast<int>(i);
							}
							else if (Inside(opts->LeftArrow(i), x, y)) opts->editor.Cycle(opts->tab, i, -1);
							else if (Inside(opts->RowRect(i), x, y) && x >= opts->ValueX()) opts->editor.Cycle(opts->tab, i, +1);
						}
						if (textRow >= 0) startEditing(textRow);
						else stopEditing();
					}
				}
				if (e.type == SDL_EVENT_MOUSE_WHEEL && opts)
				{
					float mx = 0, my = 0;
					SDL_GetMouseState(&mx, &my);
					SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);
					const auto& rows = opts->editor.Tabs()[opts->tab].rows;
					for (std::size_t i = 0; i < rows.size(); ++i)
						if (Inside(opts->RowRect(i), mx, my)) opts->editor.Cycle(opts->tab, i, e.wheel.y > 0 ? +1 : -1);
				}
				if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT)
				{
					Button* clicked = nullptr;
					for (Button* b : buttons)
					{
						if (b->pressed && b->enabled && Inside(b->rect, e.button.x, e.button.y)) clicked = b;
						b->pressed = false;
					}
					if (!clicked) continue;
					if (clicked == &close || clicked == &exitX) running = false;
					else if (clicked == &play && LaunchGame(root, config.gameExe, &launchError))
					{
						// Con descargas en curso el launcher sigue (más despacio) y se
						// cierra solo al terminar.
						if (flow->Finished()) running = false;
						else
						{
							launched = true;
							flow->SetPlaying();
						}
					}
					else if (clicked == &pause) flow->SetPaused(!st.paused);
					else if (clicked == &options)
						openOptions(0);
					else if (opts && clicked == &opts->back) closeOptions();
					else if (opts && clicked == &opts->save)
					{
						bool downloadsChanged = false;
						if (!opts->editor.Save(&downloadsChanged))
							opts->message = "No se pudo guardar (¿el archivo es de sólo lectura?).";
						else
						{
							opts->message = "Guardado. El juego lo usa la próxima vez que arranque.";
							// El tamaño de la ventana se aplica en el momento.
							std::vector<std::string> ignored;
							const int scale = loadConfig(&ignored).scale;
							if (scale != appliedScale)
							{
								appliedScale = scale;
								config.scale = scale;
								fontScale = ApplyScale(window, renderer, scale);
								reshape();
								ui.MakeFonts(fontScale);
							}
							if (downloadsChanged)
							{
								// Lo que se baja cambió: se vuelve a planear.
								flow.reset();
								warnings.clear();
								config = loadConfig(&warnings);
								config.selfPath = selfPath;
								flow = startFlow(config);
							}
						}
					}
					else if (opts && clicked == &opts->web)
					{
						// Sólo http(s): la URL la anuncia el server o viene compilada.
						if ((st.webUrl.compare(0, 7, "http://") == 0 || st.webUrl.compare(0, 8, "https://") == 0) &&
						    !SDL_OpenURL(st.webUrl.c_str()))
							opts->message = std::string("No se pudo abrir el navegador: ") + SDL_GetError();
					}
					else if (opts && clicked == &opts->verify)
					{
						flow.reset();
						Config c = config;
						c.fullVerify = true;
						flow = startFlow(c);
						closeOptions();
					}
				}
			}

			const ThemeSpec& th = g_Theme;
			// Avisos sobre la imagen (configuración de prueba, canales que faltaron).
			std::vector<std::string> notes = warnings;
			notes.insert(notes.end(), st.notices.begin(), st.notices.end());
			const float pb = th.imageBevel ? 2.0f : 0.0f;
			const SDL_FRect inner{ th.imagePanel.x + pb, th.imagePanel.y + pb, th.imagePanel.w - 2 * pb, th.imagePanel.h - 2 * pb };
			// La web tapa la imagen: sólo en la pantalla principal y sin avisos que
			// mostrar.  Mientras carga (o si no carga) queda la imagen.
			if (!st.webUrl.empty() && st.webUrl != webShownUrl)
			{
				webPanel.Navigate(st.webUrl);
				webShownUrl = st.webUrl;
			}
			const bool webWanted = !opts && notes.empty();
			{
				int pw = th.width, ph = th.height;
				SDL_GetWindowSizeInPixels(window, &pw, &ph);
				const float k = static_cast<float>(pw) / static_cast<float>(th.width);
				webPanel.Place({ static_cast<int>(std::lround(inner.x * k)), static_cast<int>(std::lround(inner.y * k)),
				                 static_cast<int>(std::lround(inner.w * k)), static_cast<int>(std::lround(inner.h * k)) },
				               webWanted);
			}
			const bool webShown = webWanted && webPanel.Loaded();
			// Con forma, lo que no tapa el fondo queda transparente.
			SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
			if (th.shaped && !opts) SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
			else SDL_SetRenderDrawColor(renderer, 224, 224, 224, 255);
			SDL_RenderClear(renderer);
			if (!opts)
			{
				// ── Pantalla principal ─────────────────────────────────────
				ui.DrawImage(th.background, nullptr);
				if (th.showTitle) ui.title->Draw(config.title, th.titlePos.x, th.titlePos.y, th.titleColor);

				if (th.imageBevel) DrawBevel(renderer, th.imagePanel);
				ui.DrawImage(th.image, &inner);

				// Noticias del server, arriba de la imagen (hasta 6 líneas).
				if (!st.news.empty() && !webShown)
				{
					std::vector<std::string> lines = Wrap(*ui.notice, st.news, inner.w - 20);
					if (lines.size() > 6)
					{
						lines.resize(6);
						lines.back() = ui.notice->Fit(lines.back() + " ...", inner.w - 20);
					}
					const float lh = ui.notice->Height() + 3;
					const SDL_FRect box{ inner.x + 4, inner.y + 4, inner.w - 8, lh * static_cast<float>(lines.size()) + 8 };
					SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
					SDL_SetRenderDrawColor(renderer, 0, 0, 0, 160);
					SDL_RenderFillRect(renderer, &box);
					for (std::size_t i = 0; i < lines.size(); ++i)
						ui.notice->Draw(lines[i], box.x + 6, box.y + 4 + lh * static_cast<float>(i), kWhite);
				}

				float ny = inner.y + inner.h - 4;
				SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
				for (auto it = notes.rbegin(); it != notes.rend() && ny > inner.y + 20; ++it)
				{
					ny -= ui.notice->Height() + 4;
					const std::string line = ui.notice->Fit(*it, inner.w - 12);
					SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
					const SDL_FRect box{ inner.x + 2, ny - 2, ui.notice->Width(line) + 8, ui.notice->Height() + 4 };
					SDL_RenderFillRect(renderer, &box);
					ui.notice->Draw(line, inner.x + 6, ny, kWhite);
				}

				// Barras de progreso: lo bajado y, si el tema tiene dos, los archivos.
				const bool done = st.phase == Phase::Ready || st.phase == Phase::Offline;
				const bool failed = st.phase == Phase::Failed;
				auto ratio = [&](std::uint64_t a, std::uint64_t b) {
					return done ? 1.0f : (b ? static_cast<float>(static_cast<double>(a) / static_cast<double>(b)) : 0.0f);
				};
				ui.DrawBar(th.bar, ratio(st.done, st.total), failed);
				ui.DrawBar(th.fileBar, ratio(st.filesDone, st.filesTotal), failed);
				std::string status = launchError.empty() ? st.message : launchError;
				if (launchError.empty() && st.paused) status = "En pausa. " + status;
				else if (launchError.empty() && st.playing && !flow->Finished() && st.waitingForClose == 0)
					status += " (jugando: más despacio)";
				const float statusW = canPause && !th.statusCentered ? pause.rect.x - th.status.x - 11 : th.status.w;
				const std::string line = ui.tiny->Fit(status, statusW);
				const float sx = th.statusCentered ? th.status.x + (th.status.w - ui.tiny->Width(line)) / 2 : th.status.x;
				ui.tiny->Draw(line, sx, th.status.y, failed || !launchError.empty() ? th.statusError : th.statusColor);
				if (canPause)
				{
					Fill(renderer, pause.rect, pause.hover ? th.pauseHover : th.pauseFill);
					DrawBevel(renderer, pause.rect, pause.pressed);
					ui.tiny->DrawCentered(pause.label, pause.rect, th.pauseText);
				}
				for (Button* b : mainButtons)
					if (b->rect.w > 0) ui.DrawButton(*b);
				ui.tiny->Draw("Diseño: kayito", th.credit.x, th.credit.y, th.creditColor);
			}
			else
			{
				// ── Opciones ───────────────────────────────────────────────
				const OptionsPalette& pal = th.palette;
				ui.DrawImage(th.optionsBackground, nullptr);
				if (th.showOptionsTitle) ui.title->Draw("Opciones del juego", th.optionsTitlePos.x, th.optionsTitlePos.y, pal.title);
				// El fondo trae recuadros fijos; las pestañas usan todo el marco.
				Fill(renderer, th.optionsPanel, pal.panel);

				const auto& tabs = opts->editor.Tabs();
				for (std::size_t i = 0; i < tabs.size(); ++i)
				{
					const SDL_FRect t = opts->TabRect(i);
					const bool on = i == opts->tab;
					Fill(renderer, t, on ? pal.tabOn : pal.tabOff);
					DrawBevel(renderer, t, !on);
					ui.tiny->DrawCentered(tabs[i].name, t, on ? pal.text : pal.dim);
				}
				const auto& rows = tabs[opts->tab].rows;
				for (std::size_t i = 0; i < rows.size(); ++i)
				{
					const SDL_FRect row = opts->RowRect(i);
					if (i % 2) Fill(renderer, row, pal.rowAlt);
					ui.regular->Draw(rows[i].label, row.x + 6, row.y + (row.h - ui.regular->Height()) / 2, pal.text);
					if (rows[i].text)
					{
						const SDL_FRect box = opts->TextBox(i);
						const bool editing = opts->editing == static_cast<int>(i);
						Fill(renderer, box, pal.box);
						DrawBevel(renderer, box);
						const std::string t = opts->editor.Text(opts->tab, i);
						ui.regular->Draw(t, box.x + 5, box.y + (box.h - ui.regular->Height()) / 2, pal.text);
						if (editing && (SDL_GetTicks() / 500) % 2 == 0)
						{
							const float cx = box.x + 5 + ui.regular->Width(t) + 1;
							SDL_SetRenderDrawColor(renderer, pal.text.r, pal.text.g, pal.text.b, 255);
							SDL_RenderLine(renderer, cx, box.y + 3, cx, box.y + box.h - 4);
						}
						else if (t.empty() && !editing)
							ui.notice->Draw("(ninguno)", box.x + 5, box.y + (box.h - ui.notice->Height()) / 2, pal.faint);
						continue;
					}
					const int cur = opts->editor.Current(opts->tab, i);
					const SDL_FRect value{ opts->ValueX() + 20, row.y, 130, row.h };
					const bool editable = opts->editor.Editable(opts->tab, i);
					ui.label->DrawCentered(rows[i].choices[static_cast<std::size_t>(cur)].label, value, editable ? pal.text : pal.dim);
					if (editable)
					{
						ui.label->DrawCentered("<", opts->LeftArrow(i), pal.faint);
						ui.label->DrawCentered(">", opts->RightArrow(i), pal.faint);
					}
				}
				// Un botón debajo de las filas, con su explicación al lado.
				auto drawExtra = [&](Button& b, const std::string& text) {
					b.rect.x = th.optionsPanel.x + 11;
					b.rect.y = opts->BelowRows(rows.size());
					ui.DrawButton(b);
					const float tx = b.rect.x + b.rect.w + 10;
					ui.notice->Draw(ui.notice->Fit(text, th.optionsPanel.x + th.optionsPanel.w - tx - 8), tx,
					                b.rect.y + (b.rect.h - ui.notice->Height()) / 2, pal.dim);
				};
				if (opts->IsDownloads()) drawExtra(opts->verify, "Vuelve a revisar todos los archivos instalados.");
				if (opts->IsGeneral())
					drawExtra(opts->web, st.webUrl.empty() ? std::string("El server no tiene sitio configurado.") : st.webUrl);
				if (!opts->message.empty())
					ui.tiny->Draw(ui.tiny->Fit(opts->message, th.optionsPanel.w - 6), th.optionsPanel.x + 3, th.messageY, pal.text);
				ui.DrawButton(opts->save);
				ui.DrawButton(opts->back);
			}
			SDL_RenderPresent(renderer);
		}
		flow.reset();   // cancela y espera la descarga en curso
	}   // texturas y fuentes antes que el renderer
	if (shapeMain) SDL_DestroySurface(shapeMain);
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
