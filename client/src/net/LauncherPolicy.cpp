#include "stdafx.h"
#include "net/LauncherPolicy.h"
#include "net/NetChannel.h"
#include "net/Protocol/ConnectServerProtocol.h"
#include "app/core/Window.h"
#include "assets/text/ClientText.h"
#include "platform/IFileSystem.h"
#include "platform/IWindow.h"
#include "platform/RunningLock.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#if !defined(_WIN32)
#include <spawn.h>
extern char** environ;
#endif

extern "C" void DbgLogPublic(const char* msg);

namespace LauncherPolicy {

namespace {

namespace fs = std::filesystem;

bool s_Handled = false;

#if defined(_WIN32)
constexpr const char* kLauncherNames[] = { "Launcher/launcher.exe", "launcher.exe" };
#else
constexpr const char* kLauncherNames[] = { "Launcher/launcher", "launcher" };
#endif

bool StartedByLauncher()
{
    const char* v = std::getenv(kStartedByLauncherEnv);
    return v && *v && std::strcmp(v, "0") != 0;
}

// El launcher va al lado del juego: en Launcher/ (donde lo instala su parche) o
// en la misma carpeta.
fs::path FindLauncher()
{
    char exe[1024] = {};
    if (Platform::GetFileSystem().ExecutablePath(exe, sizeof(exe)) == 0 || !*exe) return fs::path();
    const fs::path dir = fs::u8path(exe).parent_path();
    std::error_code ec;
    for (const char* name : kLauncherNames) {
        const fs::path candidate = dir / fs::u8path(name);
        if (fs::is_regular_file(candidate, ec)) return candidate;
    }
    return fs::path();
}

bool StartLauncher(const fs::path& path)
{
#if defined(_WIN32)
    const std::wstring app = path.wstring();
    std::wstring cmd = L"\"" + app + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(app.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr,
                        path.parent_path().wstring().c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    const std::string app = path.string();
    char* argv[] = { const_cast<char*>(app.c_str()), nullptr };
    pid_t pid = 0;
    return posix_spawn(&pid, app.c_str(), nullptr, nullptr, argv, environ) == 0;
#endif
}

} // namespace

bool Supported()
{
#if (defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__))) && !defined(__EMSCRIPTEN__)
    return true;
#else
    return false;
#endif
}

void Request()
{
    if (!Supported()) return;
    Proto::PMSG_CLIENT_POLICY_RECV req{};
    req.header = {0xC1, (std::uint8_t)sizeof(req), 0xF4, 0x16};
    NetChannel::Send((const char*)&req, sizeof(req));
}

void OnPolicy(const std::uint8_t* msg, int size)
{
    Proto::PMSG_CLIENT_POLICY_SEND head{};
    if (!Supported() || s_Handled || !msg || size < Proto::kClientPolicyBaseSize) return;
    std::memcpy(&head, msg, size < (int)sizeof(head) ? (std::size_t)size : sizeof(head));
    // Sólo lo que llega por el canal cifrado es del server.
    if (head.result != Proto::CONTENT_OK || !NetChannel::IsSecure()) return;
    if (!(head.flags & Proto::CLIENT_POLICY_REQUIRE_LAUNCHER) || StartedByLauncher()) return;
    s_Handled = true;

    const fs::path launcher = FindLauncher();
    // El launcher mira este lock para saber si el juego sigue abierto.
    RunningLock::Release();
    if (!launcher.empty() && StartLauncher(launcher)) {
        DbgLogPublic("LauncherPolicy: el server pide el launcher; se abre y se cierra el juego");
    } else {
        DbgLogPublic("LauncherPolicy: el server pide el launcher y no se pudo abrir");
        Platform::GetWindow().ShowError(gClientText.Get(ClientTextId::LauncherRequired), "Mu Online", true, true);
    }
    gWindow.RequestCloseApplication();
}

} // namespace LauncherPolicy
