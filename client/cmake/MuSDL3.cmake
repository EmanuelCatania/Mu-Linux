# SDL3 con la revisión aprobada (no se busca uno instalado en el sistema).  Lo
# usan el cliente (MuSDLDependencies) y el launcher (src/launcher).

if(TARGET SDL3::SDL3-static)
    return()
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(MU_WAYLAND REQUIRED wayland-client>=1.18 wayland-egl wayland-cursor egl xkbcommon>=0.5.0 libdecor-0)
    set(SDL_WAYLAND ON CACHE BOOL "" FORCE)
    set(SDL_X11 ON CACHE BOOL "" FORCE)
endif()
FetchContent_Declare(mu_sdl
    URL https://github.com/libsdl-org/SDL/archive/7f3ae3d57459e59943a4ecfefc8f6277ec6bf540.tar.gz)
FetchContent_MakeAvailable(mu_sdl)
