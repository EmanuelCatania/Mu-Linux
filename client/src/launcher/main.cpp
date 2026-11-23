// Launcher del cliente (Fase 5).  Ventana con el tema WebzenOldSchoolV1 de
// kayito: actualiza el cliente al abrir, "Opciones" edita User/Config.ini y
// User/launcher.toml, y "Jugar" arranca el cliente.
//
// Uso: launcher [--root=<carpeta del cliente>] [--options[=<pestaña>]]
// Sin --root, la carpeta del cliente es la del launcher.  --options abre directo
// la pantalla de opciones (0 = Pantalla ... 7 = Descargas).

#include "Gfx.h"
#include "LauncherConfig.h"
#include "OptionsModel.h"
#include "Resources.h"
#include "UpdateFlow.h"

#include "update/FileUtil.h"
#include "update/HttpTransport.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>

#ifndef _WIN32
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

SDL_HitTestResult HitTest(SDL_Window*, const SDL_Point* p, void*)
{
	return p->y < kHeaderHeight ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
}

std::string RootFromArgs(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
		if (std::strncmp(argv[i], "--root=", 7) == 0) return argv[i] + 7;
	const char* base = SDL_GetBasePath();
	return base ? base : ".";
}

bool LaunchGame(const std::string& root, const std::string& exe, std::string* error)
{
	const std::string path = Update::JoinPath(root, exe);
	if (!Update::FileExists(path))
	{
		*error = "No está " + exe + ".";
		return false;
	}
#ifndef _WIN32
	::chmod(path.c_str(), 0755);
#endif
	const char* args[] = { path.c_str(), nullptr };
	SDL_PropertiesID props = SDL_CreateProperties();
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, const_cast<char**>(args));
	SDL_SetBooleanProperty(props, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
	SDL_Environment* env = nullptr;
#ifndef _WIN32
	// El cliente de Linux toma sus carpetas de acá (ver platform/UserPath.h).
	env = SDL_CreateEnvironment(true);
	SDL_SetEnvironmentVariable(env, "MU_CLIENT_ROOT", root.c_str(), true);
	SDL_SetEnvironmentVariable(env, "MU_ASSET_ROOT", root.c_str(), true);
	SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ENVIRONMENT_POINTER, env);
#endif
	SDL_Process* proc = SDL_CreateProcessWithProperties(props);
	SDL_DestroyProperties(props);
	if (env) SDL_DestroyEnvironment(env);
	if (!proc)
	{
		*error = std::string("No se pudo abrir el juego: ") + SDL_GetError();
		return false;
	}
	SDL_DestroyProcess(proc);   // no lo cierra: sólo suelta el objeto
	return true;
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
	bool IsDownloads() const { return editor.Tabs()[tab].name == "Descargas"; }
};

} // namespace

int main(int argc, char** argv)
{
	std::string root = RootFromArgs(argc, argv);
	std::error_code ec;
	std::filesystem::current_path(std::filesystem::u8path(root), ec);
	const std::string userDir = UserDirFor(root);
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

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Launcher", SDL_GetError(), nullptr);
		return 1;
	}
	SDL_Window* window = SDL_CreateWindow(config.title.c_str(), kWidth, kHeight, SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIGH_PIXEL_DENSITY);
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
		ui.title = std::make_unique<Font>(renderer, FontBold, FontBoldSize, 13.0f);
		ui.label = std::make_unique<Font>(renderer, FontBold, FontBoldSize, 12.5f);
		ui.regular = std::make_unique<Font>(renderer, FontRegular, FontRegularSize, 12.5f);
		ui.tiny = std::make_unique<Font>(renderer, FontBold, FontBoldSize, 10.0f);
		ui.notice = std::make_unique<Font>(renderer, FontRegular, FontRegularSize, 11.0f);

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
		auto openOptions = [&](std::size_t tab) {
			opts = std::make_unique<OptionsScreen>();
			if (!opts->editor.Load(iniPath, iniTemplate, tomlPath)) opts->message = "No se pudo leer Config.ini.";
			if (tab < opts->editor.Tabs().size()) opts->tab = tab;
		};
		for (int i = 1; i < argc; ++i)
			if (std::strncmp(argv[i], "--options", 9) == 0)
				openOptions(argv[i][9] == '=' ? static_cast<std::size_t>(std::atoi(argv[i] + 10)) : 0);
		bool running = true;
		while (running)
		{
			const Status st = flow->Snapshot();
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
				if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE)
				{
					if (opts) opts.reset();
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
						for (std::size_t i = 0; i < opts->editor.Tabs().size(); ++i)
							if (Inside(opts->TabRect(i), x, y)) opts->tab = i;
						const auto& rows = opts->editor.Tabs()[opts->tab].rows;
						for (std::size_t i = 0; i < rows.size(); ++i)
						{
							if (Inside(opts->LeftArrow(i), x, y)) opts->editor.Cycle(opts->tab, i, -1);
							else if (Inside(opts->RowRect(i), x, y) && x >= 262) opts->editor.Cycle(opts->tab, i, +1);
						}
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
					else if (opts && clicked == &opts->back) opts.reset();
					else if (opts && clicked == &opts->save)
					{
						bool downloadsChanged = false;
						if (!opts->editor.Save(&downloadsChanged))
							opts->message = "No se pudo guardar (¿el archivo es de sólo lectura?).";
						else
						{
							opts->message = "Guardado. El juego lo usa la próxima vez que arranque.";
							if (downloadsChanged)
							{
								// Lo que se baja cambió: se vuelve a planear.
								flow.reset();
								warnings.clear();
								config = loadConfig(&warnings);
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
						opts.reset();
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

				// Avisos sobre la imagen (configuración de prueba, canales que faltaron).
				std::vector<std::string> notes = warnings;
				notes.insert(notes.end(), st.notices.begin(), st.notices.end());
				if (st.newLauncher) notes.push_back("Hay un launcher nuevo; esta versión todavía no se actualiza sola.");
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
