#pragma once
#include "platform/IDiagnostics.h"
#ifdef _WIN32
#include "platform/Win32/Win32Diagnostics.h"
#elif defined(__EMSCRIPTEN__)
#include "platform/Web/WebDiagnostics.h"
#else
#include "platform/Posix/PosixDiagnostics.h"
#endif
namespace Platform {
inline IDiagnostics& GetDiagnostics() {
#ifdef _WIN32
    static Win32Diagnostics diagnostics;
#elif defined(__EMSCRIPTEN__)
    static WebDiagnostics diagnostics;
#else
    static PosixDiagnostics diagnostics;
#endif
    return diagnostics;
}
}
