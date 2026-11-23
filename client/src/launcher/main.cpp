// Launcher del cliente (Fase 5).  Ventana con el tema WebzenOldSchoolV1 de
// kayito: actualiza el cliente al abrir y "Jugar" lo arranca.
//
// Uso: launcher [--root=<carpeta del cliente>]
// Sin --root, la carpeta del cliente es la del launcher.

#include "Gfx.h"
#include "LauncherConfig.h"
#include "Resources.h"
#include "UpdateFlow.h"

#include "update/FileUtil.h"
#include "update/HttpTransport.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

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
const SDL_Color kWhite{ 255, 255, 255, 255 };

struct Button
{
	SDL_FRect rect;
	const char* label;
	bool enabled = true;
	bool hover = false;
	bool pressed = false;

	bool Contains(float x, float y) const
	{
		return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
	}
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

void DrawBevel(SDL_Renderer* r, const SDL_FRect& box)
{
	// Borde hundido, como el Fixed3D del original.
	SDL_SetRenderDrawColor(r, 128, 128, 128, 255);
	SDL_RenderLine(r, box.x, box.y, box.x + box.w - 1, box.y);
	SDL_RenderLine(r, box.x, box.y, box.x, box.y + box.h - 1);
	SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
	SDL_RenderLine(r, box.x + box.w - 1, box.y, box.x + box.w - 1, box.y + box.h - 1);
	SDL_RenderLine(r, box.x, box.y + box.h - 1, box.x + box.w - 1, box.y + box.h - 1);
}

} // namespace

int main(int argc, char** argv)
{
	std::string root = RootFromArgs(argc, argv);
	std::error_code ec;
	std::filesystem::current_path(std::filesystem::u8path(root), ec);

	Config config = CompiledConfig();
	std::vector<std::string> warnings;
	ApplyLauncherToml(Update::JoinPath(UserDirFor(root), "launcher.toml"), config, &warnings);

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
	SDL_Texture* background = LoadPng(renderer, MainBackground, MainBackgroundSize);
	SDL_Texture* image = LoadPng(renderer, MainImage, MainImageSize);
	SDL_Texture* btnNormal = LoadPng(renderer, ButtonNormal, ButtonNormalSize);
	SDL_Texture* btnHover = LoadPng(renderer, ButtonHover, ButtonHoverSize);
	SDL_Texture* btnDisabled = LoadPng(renderer, ButtonDisabled, ButtonDisabledSize);
	Font title(renderer, FontBold, FontBoldSize, 13.0f);
	Font label(renderer, FontBold, FontBoldSize, 12.5f);
	Font tiny(renderer, FontBold, FontBoldSize, 10.0f);
	Font notice(renderer, FontRegular, FontRegularSize, 11.0f);

	// Botones en la posición del original (panel en 96,412 + cada botón).
	Button options{ { 110, 417, 67, 31 }, "Opciones" };
	Button play{ { 191, 417, 67, 31 }, "Jugar" };
	Button close{ { 272, 417, 67, 31 }, "Cerrar" };
	options.enabled = false;   // las opciones llegan en el paso siguiente
	Button* buttons[] = { &options, &play, &close };

	auto flow = std::make_unique<UpdateFlow>(config, root, std::make_unique<AnyTransport>(std::make_unique<Update::HttpTransport>()));
	flow->Start();
	std::string launchError;

	bool running = true;
	while (running)
	{
		const Status st = flow->Snapshot();
		play.enabled = st.canPlay;

		SDL_Event e;
		while (SDL_PollEvent(&e))
		{
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) running = false;
			if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) running = false;
			if (e.type == SDL_EVENT_MOUSE_MOTION || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP)
				SDL_ConvertEventToRenderCoordinates(renderer, &e);
			if (e.type == SDL_EVENT_MOUSE_MOTION)
				for (Button* b : buttons) b->hover = b->Contains(e.motion.x, e.motion.y);
			if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN && e.button.button == SDL_BUTTON_LEFT)
				for (Button* b : buttons) b->pressed = b->enabled && b->Contains(e.button.x, e.button.y);
			if (e.type == SDL_EVENT_MOUSE_BUTTON_UP && e.button.button == SDL_BUTTON_LEFT)
			{
				for (Button* b : buttons)
				{
					const bool clicked = b->pressed && b->enabled && b->Contains(e.button.x, e.button.y);
					b->pressed = false;
					if (!clicked) continue;
					if (b == &close) running = false;
					if (b == &play && LaunchGame(root, config.gameExe, &launchError)) running = false;
				}
			}
		}

		SDL_SetRenderDrawColor(renderer, 224, 224, 224, 255);
		SDL_RenderClear(renderer);
		if (background) SDL_RenderTexture(renderer, background, nullptr, nullptr);
		title.Draw(config.title, 5, 6, kBlack);

		const SDL_FRect panel{ 6, 31, 437, 327 };
		DrawBevel(renderer, panel);
		const SDL_FRect inner{ panel.x + 2, panel.y + 2, panel.w - 4, panel.h - 4 };
		if (image) SDL_RenderTexture(renderer, image, nullptr, &inner);

		// Avisos sobre la imagen (configuración de prueba, canales que faltaron).
		std::vector<std::string> notes = warnings;
		notes.insert(notes.end(), st.notices.begin(), st.notices.end());
		if (st.newLauncher) notes.push_back("Hay un launcher nuevo; esta versión todavía no se actualiza sola.");
		float ny = inner.y + inner.h - 4;
		for (auto it = notes.rbegin(); it != notes.rend() && ny > inner.y + 20; ++it)
		{
			ny -= notice.Height() + 4;
			const std::string line = notice.Fit(*it, inner.w - 12);
			SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
			SDL_SetRenderDrawColor(renderer, 0, 0, 0, 170);
			const SDL_FRect box{ inner.x + 2, ny - 2, notice.Width(line) + 8, notice.Height() + 4 };
			SDL_RenderFillRect(renderer, &box);
			notice.Draw(line, inner.x + 6, ny, kWhite);
		}

		// Barra de progreso.
		const SDL_FRect bar{ 6, 362, 437, 22 };
		DrawBevel(renderer, bar);
		const SDL_FRect track{ bar.x + 2, bar.y + 2, bar.w - 4, bar.h - 4 };
		SDL_SetRenderDrawColor(renderer, 240, 240, 240, 255);
		SDL_RenderFillRect(renderer, &track);
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
		tiny.Draw(tiny.Fit(status, 437), 6, 388, st.phase == Phase::Failed || !launchError.empty() ? SDL_Color{ 150, 0, 0, 255 } : kBlack);

		for (Button* b : buttons)
		{
			SDL_Texture* t = !b->enabled ? btnDisabled : (b->hover ? btnHover : btnNormal);
			if (t) SDL_RenderTexture(renderer, t, nullptr, &b->rect);
			SDL_FRect textBox = b->rect;
			if (b->pressed) textBox.y += 1;
			label.DrawCentered(b->label, textBox, b->enabled ? kBlack : kGray);
		}
		tiny.Draw("Diseño: kayito", 350, 442, kGray);

		SDL_RenderPresent(renderer);
	}

	flow.reset();   // cancela y espera la descarga en curso
	for (SDL_Texture* t : { background, image, btnNormal, btnHover, btnDisabled })
		if (t) SDL_DestroyTexture(t);
	}   // las fuentes sueltan su textura antes que el renderer
	SDL_DestroyRenderer(renderer);
	SDL_DestroyWindow(window);
	SDL_Quit();
	return 0;
}
