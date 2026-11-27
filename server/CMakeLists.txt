cmake_minimum_required(VERSION 3.21)
project(MuServer LANGUAGES C CXX)
# Sólo 64 bits: Windows x64, Linux x64 y ARM64.  Los builds x86 (MSVC, MinGW y
# la solución MSBuild del MuEmu original) se quitaron.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "El server se compila sólo en 64 bits: usá windows-x64-*, linux-x64-*, linux-arm64-* o mingw-x64-release")
endif()
add_subdirectory(Source/MuServer)
