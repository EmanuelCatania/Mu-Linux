#pragma once
#include "platform/IIdentity.h"
#ifdef _WIN32
#include "platform/Win32/Win32Identity.h"
#elif defined(__EMSCRIPTEN__)
#include "platform/Web/WebIdentity.h"
#else
#include "platform/Posix/PosixIdentity.h"
#endif
namespace Platform {
inline IIdentity& GetIdentity() {
#ifdef _WIN32
    static Win32Identity identity;
#elif defined(__EMSCRIPTEN__)
    static WebIdentity identity;
#else
    static PosixIdentity identity;
#endif
    return identity;
}
}
