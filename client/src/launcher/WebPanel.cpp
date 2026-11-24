#include "WebPanel.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

#ifdef MU_WEBVIEW2
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h>
#include <wrl.h>

#include <WebView2.h>
#endif

namespace Launcher {

namespace {

[[maybe_unused]] bool IsHttp(const std::string& url)
{
	return url.compare(0, 7, "http://") == 0 || url.compare(0, 8, "https://") == 0;
}

// "https://Mu.Ejemplo.com:8080/x" -> "mu.ejemplo.com"
[[maybe_unused]] std::string HostOf(const std::string& url)
{
	const std::size_t s = url.find("://");
	if (s == std::string::npos) return std::string();
	std::size_t b = s + 3;
	const std::size_t at = url.find('@', b);
	const std::size_t slash = url.find_first_of("/?#", b);
	if (at != std::string::npos && (slash == std::string::npos || at < slash)) b = at + 1;
	const std::size_t e = url.find_first_of(":/?#", b);
	std::string h = url.substr(b, e == std::string::npos ? std::string::npos : e - b);
	std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return h;
}

} // namespace

#ifdef MU_WEBVIEW2

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring Wide(const std::string& s)
{
	if (s.empty()) return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<std::size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string Narrow(const wchar_t* w)
{
	if (!w || !*w) return std::string();
	const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	std::string s(static_cast<std::size_t>(n > 0 ? n - 1 : 0), '\0');
	if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
	return s;
}

// Lo que no es del sitio del server va al navegador del sistema.
void OpenOutside(const std::string& url)
{
	if (IsHttp(url)) SDL_OpenURL(url.c_str());
}

} // namespace

struct WebPanel::Impl
{
	HWND parent = nullptr;
	bool comInit = false;
	ComPtr<ICoreWebView2Controller> controller;
	ComPtr<ICoreWebView2> view;
	std::string pending;        // a dónde ir cuando el control esté listo
	std::string host;           // el sitio del server
	bool loaded = false;
	bool visible = false;
	SDL_Rect bounds{ 0, 0, 0, 0 };

	void Go(const std::string& url)
	{
		host = HostOf(url);
		loaded = false;
		if (view) view->Navigate(Wide(url).c_str());
		else pending = url;
	}

	void Apply()
	{
		if (!controller) return;
		const RECT r{ bounds.x, bounds.y, bounds.x + bounds.w, bounds.y + bounds.h };
		controller->put_Bounds(r);
		controller->put_IsVisible(visible && loaded ? TRUE : FALSE);
	}

	void Ready(ICoreWebView2Controller* c)
	{
		controller = c;
		if (FAILED(controller->get_CoreWebView2(&view)) || !view)
		{
			controller.Reset();
			return;
		}
		controller->put_IsVisible(FALSE);
		ComPtr<ICoreWebView2Settings> settings;
		if (SUCCEEDED(view->get_Settings(&settings)) && settings)
		{
			settings->put_AreDefaultContextMenusEnabled(FALSE);
			settings->put_AreDevToolsEnabled(FALSE);
			settings->put_IsStatusBarEnabled(FALSE);
			settings->put_IsZoomControlEnabled(FALSE);
		}
		EventRegistrationToken token{};
		// Sólo se navega dentro del sitio; el resto, al navegador.
		view->add_NavigationStarting(
			Callback<ICoreWebView2NavigationStartingEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
					LPWSTR uri = nullptr;
					if (FAILED(args->get_Uri(&uri))) return S_OK;
					const std::string url = Narrow(uri);
					CoTaskMemFree(uri);
					// Una redirección antes de la primera carga (http -> https,
					// sin www -> con www) define el sitio; si no, el panel no
					// cargaría nunca y cada arranque abriría el navegador.
					BOOL redirected = FALSE;
					args->get_IsRedirected(&redirected);
					if (redirected && !loaded && IsHttp(url))
					{
						host = HostOf(url);
						return S_OK;
					}
					if (!IsHttp(url) || HostOf(url) != host)
					{
						args->put_Cancel(TRUE);
						OpenOutside(url);
					}
					return S_OK;
				})
				.Get(),
			&token);
		view->add_NewWindowRequested(
			Callback<ICoreWebView2NewWindowRequestedEventHandler>(
				[](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
					LPWSTR uri = nullptr;
					if (SUCCEEDED(args->get_Uri(&uri)))
					{
						OpenOutside(Narrow(uri));
						CoTaskMemFree(uri);
					}
					args->put_Handled(TRUE);
					return S_OK;
				})
				.Get(),
			&token);
		view->add_NavigationCompleted(
			Callback<ICoreWebView2NavigationCompletedEventHandler>(
				[this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
					BOOL ok = FALSE;
					args->get_IsSuccess(&ok);
					if (ok) loaded = true;
					Apply();
					return S_OK;
				})
				.Get(),
			&token);
		Apply();
		if (!pending.empty())
		{
			const std::string url = pending;
			pending.clear();
			Go(url);
		}
	}
};

WebPanel::WebPanel(SDL_Window* window, const std::string& userDataDir) : m(std::make_unique<Impl>())
{
	m->parent = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
	if (!m->parent) return;
	const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	m->comInit = SUCCEEDED(co);
	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(userDataDir), ec);
	Impl* impl = m.get();
	// Si no está el runtime de WebView2 esto falla y no hay panel web.
	CreateCoreWebView2EnvironmentWithOptions(
		nullptr, Wide(userDataDir).c_str(), nullptr,
		Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
			[impl](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
				if (FAILED(result) || !env) return S_OK;
				env->CreateCoreWebView2Controller(
					impl->parent,
					Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
						[impl](HRESULT r, ICoreWebView2Controller* c) -> HRESULT {
							if (SUCCEEDED(r) && c) impl->Ready(c);
							return S_OK;
						})
						.Get());
				return S_OK;
			})
			.Get());
}

WebPanel::~WebPanel()
{
	if (m->controller) m->controller->Close();
	m->view.Reset();
	m->controller.Reset();
	if (m->comInit) CoUninitialize();
}

bool WebPanel::Available() { return true; }

void WebPanel::Navigate(const std::string& url)
{
	if (IsHttp(url)) m->Go(url);
}

bool WebPanel::Loaded() const { return m->loaded; }

void WebPanel::Place(const SDL_Rect& pixels, bool show)
{
	if (pixels.x == m->bounds.x && pixels.y == m->bounds.y && pixels.w == m->bounds.w && pixels.h == m->bounds.h &&
	    show == m->visible)
		return;
	m->bounds = pixels;
	m->visible = show;
	m->Apply();
}

#else

struct WebPanel::Impl
{
};

WebPanel::WebPanel(SDL_Window*, const std::string&) : m(std::make_unique<Impl>()) {}
WebPanel::~WebPanel() = default;
bool WebPanel::Available() { return false; }
void WebPanel::Navigate(const std::string&) {}
bool WebPanel::Loaded() const { return false; }
void WebPanel::Place(const SDL_Rect&, bool) {}

#endif

} // namespace Launcher
