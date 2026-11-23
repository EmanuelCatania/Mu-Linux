// Launcher del cliente (Fase 5).  Ventana con el tema WebzenOldSchoolV1 de
// kayito: actualiza el cliente al abrir, "Opciones" edita User/Config.ini y
// User/launcher.toml, y "Jugar" arranca el cliente.
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
#include "UpdateFlow.h"

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

constexpr int kWidth = 450;
constexpr int kHeight = 458;
constexpr float kHeaderHeight = 26;

const SDL_Color kBlack{ 0, 0, 0, 255 };
const SDL_Color kGray{ 70, 70, 70, 255 };
const SDL_Color kLightGray{ 120, 120, 120, 255 };
const SDL_Color kWhite{ 255, 255, 255, 255 };
const SDL_Color kRed{ 150, 0, 0, 255 };

bool Inside(const SDL_FRect& r, float x, float y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

struct Button
{
	SDL_FRect rect;
	const char* label;
	bool enabled = true;
	bool hover = false;
	bool pressed = false;
};

// Unidades de ventana por unidad lógica (la ventana se agranda con "Tamaño del
// launcher"; el dibujo sigue en 450x458).
float g_WindowScale = 1.0f;

SDL_HitTestResult HitTest(SDL_Window*, const SDL_Point* p, void*)
{
	return p->y < kHeaderHeight * g_WindowScale ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
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
		const float fit = std::min(static_cast<float>(area.w) / kWidth, static_cast<float>(area.h) / kHeight) * 0.95f;
		if (s > fit) s = std::max(1.0f, fit);
	}
	return s;
}

// Cambia el tamaño de la ventana y la centra.  Devuelve los píxeles por unidad
// lógica, para rasterizar las fuentes a ese tamaño.
float ApplyScale(SDL_Window* window, SDL_Renderer* renderer, int percent)
{
	const float s = ChooseScale(window, percent);
	const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
	SDL_SetWindowSize(window, static_cast<int>(std::lround(kWidth * s)), static_cast<int>(std::lround(kHeight * s)));
	SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED_DISPLAY(display), SDL_WINDOWPOS_CENTERED_DISPLAY(display));
	SDL_SyncWindow(window);
	int ww = kWidth, wh = kHeight, ow = kWidth, oh = kHeight;
	SDL_GetWindowSize(window, &ww, &wh);
	SDL_GetRenderOutputSize(renderer, &ow, &oh);
	g_WindowScale = std::min(static_cast<float>(ww) / kWidth, static_cast<float>(wh) / kHeight);
	const float px = std::min(static_cast<float>(ow) / kWidth, static_cast<float>(oh) / kHeight);
	return px > 1.0f ? px : 1.0f;
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

void Fill(SDL_Renderer* r, const SDL_FRect& box, Uint8 gray)
{
	SDL_SetRenderDrawColor(r, gray, gray, gray, 255);
	SDL_RenderFillRect(r, &box);
}

struct Ui
{
	SDL_Renderer* r = nullptr;
	SDL_Texture* background = nullptr;
	SDL_Texture* optionsBackground = nullptr;
	SDL_Texture* image = nullptr;
	SDL_Texture* btnNormal = nullptr;
	SDL_Texture* btnHover = nullptr;
	SDL_Texture* btnDisabled = nullptr;
	std::unique_ptr<Font> title, label, regular, tiny, notice;

	void MakeFonts(float scale)
	{
		using namespace LauncherRes;
		title = std::make_unique<Font>(r, FontBold, FontBoldSize, 13.0f, scale);
		label = std::make_unique<Font>(r, FontBold, FontBoldSize, 12.5f, scale);
		regular = std::make_unique<Font>(r, FontRegular, FontRegularSize, 12.5f, scale);
		tiny = std::make_unique<Font>(r, FontBold, FontBoldSize, 10.0f, scale);
		notice = std::make_unique<Font>(r, FontRegular, FontRegularSize, 11.0f, scale);
	}

	void DrawButton(const Button& b) const
	{
		SDL_Texture* t = !b.enabled ? btnDisabled : (b.hover ? btnHover : btnNormal);
		if (t) SDL_RenderTexture(r, t, nullptr, &b.rect);
		SDL_FRect box = b.rect;
		if (b.pressed) box.y += 1;
		label->DrawCentered(b.label, box, b.enabled ? kBlack : kGray);
	}

	~Ui()
	{
		for (SDL_Texture* t : { background, optionsBackground, image, btnNormal, btnHover, btnDisabled })
			if (t) SDL_DestroyTexture(t);
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
	Button save{ { 150, 417, 67, 31 }, "Guardar" };
	Button back{ { 233, 417, 67, 31 }, "Volver" };
	Button verify{ { 20, 0, 150, 26 }, "Verificar archivos" };

	static constexpr float kTabY = 36, kTabH = 20, kTabW = 106, kRowY = 86, kRowH = 23;

	SDL_FRect TabRect(std::size_t i) const
	{
		return { 12 + static_cast<float>(i % 4) * (kTabW + 1), kTabY + static_cast<float>(i / 4) * (kTabH + 2), kTabW, kTabH };
	}
	SDL_FRect RowRect(std::size_t i) const { return { 14, kRowY + static_cast<float>(i) * kRowH, 422, kRowH - 2 }; }
	SDL_FRect LeftArrow(std::size_t i) const { const SDL_FRect r = RowRect(i); return { 262, r.y, 20, r.h }; }
	SDL_FRect RightArrow(std::size_t i) const { const SDL_FRect r = RowRect(i); return { 412, r.y, 20, r.h }; }
	SDL_FRect TextBox(std::size_t i) const { const SDL_FRect r = RowRect(i); return { 262, r.y + 1, 170, r.h - 2 }; }
	bool IsDownloads() const { return editor.Tabs()[tab].name == "Descargas"; }
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
	SDL_Window* window = SDL_CreateWindow(config.title.c_str(), kWidth, kHeight,
	                                      SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
	SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
	if (!renderer)
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launcher", SDL_GetError(), nullptr);
		SDL_Quit();
		return 1;
	}
	SDL_SetRenderLogicalPresentation(renderer, kWidth, kHeight, SDL_LOGICAL_PRESENTATION_LETTERBOX);
	SDL_SetRenderVSync(renderer, 1);
	SDL_SetWindowHitTest(window, HitTest, nullptr);
	int appliedScale = config.scale;
	float fontScale = ApplyScale(window, renderer, appliedScale);
	SDL_ShowWindow(window);

	{
		using namespace LauncherRes;
		Ui ui;
		ui.r = renderer;
		ui.background = LoadPng(renderer, MainBackground, MainBackgroundSize);
		ui.optionsBackground = LoadPng(renderer, OptionsBackground, OptionsBackgroundSize);
		ui.image = LoadPng(renderer, MainImage, MainImageSize);
		ui.btnNormal = LoadPng(renderer, ButtonNormal, ButtonNormalSize);
		ui.btnHover = LoadPng(renderer, ButtonHover, ButtonHoverSize);
		ui.btnDisabled = LoadPng(renderer, ButtonDisabled, ButtonDisabledSize);
		ui.MakeFonts(fontScale);

		// Botones en la posición del original (panel en 96,412 + cada botón).
		Button options{ { 110, 417, 67, 31 }, "Opciones" };
		Button play{ { 191, 417, 67, 31 }, "Jugar" };
		Button close{ { 272, 417, 67, 31 }, "Cerrar" };

		auto startFlow = [&](const Config& c) {
			auto f = std::make_unique<UpdateFlow>(c, root, std::make_unique<AnyTransport>(std::make_unique<Update::HttpTransport>()));
			f->Start();
			return f;
		};
		std::unique_ptr<UpdateFlow> flow = startFlow(config);
		std::string launchError;

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
		};
		auto openOptions = [&](std::size_t tab) {
			opts = std::make_unique<OptionsScreen>();
			if (!opts->editor.Load(iniPath, iniTemplate, tomlPath)) opts->message = "No se pudo leer Config.ini.";
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
			Button* mainButtons[] = { &options, &play, &close };
			std::vector<Button*> buttons;
			if (opts)
			{
				opts->save.enabled = opts->editor.Dirty();
				opts->verify.enabled = flow->Finished();
				buttons = { &opts->save, &opts->back };
				if (opts->IsDownloads()) buttons.push_back(&opts->verify);
			}
			else
				buttons.assign(std::begin(mainButtons), std::end(mainButtons));

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
								if (Inside(opts->RowRect(i), x, y)) textRow = static_cast<int>(i);
							}
							else if (Inside(opts->LeftArrow(i), x, y)) opts->editor.Cycle(opts->tab, i, -1);
							else if (Inside(opts->RowRect(i), x, y) && x >= 262) opts->editor.Cycle(opts->tab, i, +1);
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
					if (clicked == &close) running = false;
					else if (clicked == &play && LaunchGame(root, config.gameExe, &launchError)) running = false;
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

			Fill(renderer, { 0, 0, kWidth, kHeight }, 224);
			if (!opts)
			{
				// ── Pantalla principal ─────────────────────────────────────
				if (ui.background) SDL_RenderTexture(renderer, ui.background, nullptr, nullptr);
				ui.title->Draw(config.title, 5, 6, kBlack);

				const SDL_FRect panel{ 6, 31, 437, 327 };
				DrawBevel(renderer, panel);
				const SDL_FRect inner{ panel.x + 2, panel.y + 2, panel.w - 4, panel.h - 4 };
				if (ui.image) SDL_RenderTexture(renderer, ui.image, nullptr, &inner);

				// Noticias del server, arriba de la imagen (hasta 6 líneas).
				if (!st.news.empty())
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

				// Avisos sobre la imagen (configuración de prueba, canales que faltaron).
				std::vector<std::string> notes = warnings;
				notes.insert(notes.end(), st.notices.begin(), st.notices.end());
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

				// Barra de progreso.
				const SDL_FRect bar{ 6, 362, 437, 22 };
				DrawBevel(renderer, bar);
				const SDL_FRect track{ bar.x + 2, bar.y + 2, bar.w - 4, bar.h - 4 };
				Fill(renderer, track, 240);
				float frac = 0;
				if (st.phase == Phase::Ready || st.phase == Phase::Offline) frac = 1;
				else if (st.total) frac = static_cast<float>(static_cast<double>(st.done) / static_cast<double>(st.total));
				if (frac > 0)
				{
					const SDL_FRect fill{ track.x, track.y, track.w * (frac > 1 ? 1 : frac), track.h };
					if (st.phase == Phase::Failed) SDL_SetRenderDrawColor(renderer, 190, 40, 40, 255);
					else SDL_SetRenderDrawColor(renderer, 0, 120, 215, 255);
					SDL_RenderFillRect(renderer, &fill);
				}
				const std::string status = launchError.empty() ? st.message : launchError;
				ui.tiny->Draw(ui.tiny->Fit(status, 437), 6, 388, st.phase == Phase::Failed || !launchError.empty() ? kRed : kBlack);
				for (Button* b : mainButtons) ui.DrawButton(*b);
				ui.tiny->Draw("Diseño: kayito", 350, 442, kGray);
			}
			else
			{
				// ── Opciones ───────────────────────────────────────────────
				if (ui.optionsBackground) SDL_RenderTexture(renderer, ui.optionsBackground, nullptr, nullptr);
				ui.title->Draw("Opciones del juego", 6, 6, kBlack);
				// El fondo trae cuatro recuadros fijos; las pestañas usan todo el marco.
				Fill(renderer, { 9, 33, 433, 364 }, 224);

				const auto& tabs = opts->editor.Tabs();
				for (std::size_t i = 0; i < tabs.size(); ++i)
				{
					const SDL_FRect t = opts->TabRect(i);
					const bool on = i == opts->tab;
					Fill(renderer, t, on ? 250 : 205);
					DrawBevel(renderer, t, !on);
					ui.tiny->DrawCentered(tabs[i].name, t, on ? kBlack : kGray);
				}
				const auto& rows = tabs[opts->tab].rows;
				for (std::size_t i = 0; i < rows.size(); ++i)
				{
					const SDL_FRect row = opts->RowRect(i);
					if (i % 2) Fill(renderer, row, 232);
					ui.regular->Draw(rows[i].label, row.x + 6, row.y + (row.h - ui.regular->Height()) / 2, kBlack);
					if (rows[i].text)
					{
						const SDL_FRect box = opts->TextBox(i);
						const bool editing = opts->editing == static_cast<int>(i);
						Fill(renderer, box, 255);
						DrawBevel(renderer, box);
						const std::string t = opts->editor.Text(opts->tab, i);
						ui.regular->Draw(t, box.x + 5, box.y + (box.h - ui.regular->Height()) / 2, kBlack);
						if (editing && (SDL_GetTicks() / 500) % 2 == 0)
						{
							const float cx = box.x + 5 + ui.regular->Width(t) + 1;
							SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
							SDL_RenderLine(renderer, cx, box.y + 3, cx, box.y + box.h - 4);
						}
						else if (t.empty() && !editing)
							ui.notice->Draw("(ninguno)", box.x + 5, box.y + (box.h - ui.notice->Height()) / 2, kLightGray);
						continue;
					}
					const int cur = opts->editor.Current(opts->tab, i);
					const SDL_FRect value{ 282, row.y, 130, row.h };
					ui.label->DrawCentered(rows[i].choices[static_cast<std::size_t>(cur)].label, value, kBlack);
					ui.label->DrawCentered("<", opts->LeftArrow(i), kLightGray);
					ui.label->DrawCentered(">", opts->RightArrow(i), kLightGray);
				}
				if (opts->IsDownloads())
				{
					opts->verify.rect.y = OptionsScreen::kRowY + static_cast<float>(rows.size()) * OptionsScreen::kRowH + 10;
					ui.DrawButton(opts->verify);
					ui.notice->Draw("Vuelve a revisar todos los archivos instalados.", 180,
					                opts->verify.rect.y + (opts->verify.rect.h - ui.notice->Height()) / 2, kGray);
				}
				if (!opts->message.empty()) ui.tiny->Draw(ui.tiny->Fit(opts->message, 430), 12, 402, kBlack);
				ui.DrawButton(opts->save);
				ui.DrawButton(opts->back);
			}
			SDL_RenderPresent(renderer);
		}
		flow.reset();   // cancela y espera la descarga en curso
	}   // texturas y fuentes antes que el renderer
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
