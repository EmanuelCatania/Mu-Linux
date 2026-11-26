# El cliente tiene un solo backend de plataforma, SDL3, y sólo se compila en
# 64 bits (el navegador es wasm32, aparte).  El backend Win32 del 0.97k y los
# builds x86 se quitaron: confundían a quien compilaba por primera vez y nadie
# los probaba.  La variable queda por compatibilidad con scripts que la pasan.
set(MU_PLATFORM_BACKEND "SDL3" CACHE STRING "Backend de plataforma del cliente (sólo SDL3)")
if(NOT MU_PLATFORM_BACKEND STREQUAL "SDL3")
    message(FATAL_ERROR "El único backend del cliente es SDL3 (el backend Win32 se quitó)")
endif()
option(MU_PLATFORM_ONLY "Compilar sólo la biblioteca de plataforma" OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 4 AND NOT EMSCRIPTEN)
    message(FATAL_ERROR "El cliente se compila sólo en 64 bits: usá los presets windows-sdl3-x64-release o linux-x64-release")
endif()
if(NOT MU_PLATFORM_ONLY)
    if(WIN32)
        if(NOT MSVC AND NOT MINGW)
            message(FATAL_ERROR "El ejecutable Windows requiere MSVC o MinGW-w64")
        endif()
        enable_language(RC)
    else()
        message(STATUS "Ejecutable Linux en integración: aún quedan dependencias nativas del cliente")
    endif()
endif()
