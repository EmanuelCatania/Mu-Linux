# mu-patch: arma y verifica los canales de parches del launcher.
#
# Se puede compilar solo (no necesita SDL ni el resto del cliente):
#   cmake -S client/tools/mu-patch -B build/mu-patch -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/mu-patch --config Release
# o desde el CMakeLists del cliente, que lo incluye con add_subdirectory.

cmake_minimum_required(VERSION 3.25)
project(mu_patch LANGUAGES C CXX)

if(NOT DEFINED MU_CLIENT_SOURCE_DIR)
    get_filename_component(MU_CLIENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
list(APPEND CMAKE_MODULE_PATH "${MU_CLIENT_SOURCE_DIR}/cmake")
include(MuUpdate)

find_package(Threads REQUIRED)

add_library(mu_patch_core STATIC PatchBuilder.cpp PatchBuilder.h)
target_include_directories(mu_patch_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(mu_patch_core PUBLIC mu_update_core Threads::Threads)

add_executable(mu-patch main.cpp)
target_link_libraries(mu-patch PRIVATE mu_patch_core)
if(WIN32)
    target_link_libraries(mu-patch PRIVATE shell32)
endif()
if(MSVC)
    target_compile_options(mu_patch_core PRIVATE /utf-8)
    target_compile_options(mu-patch PRIVATE /utf-8)
endif()

# Pruebas: sólo si se compila solo (desde el cliente las registra su CMakeLists).
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_executable(mu_update_test "${MU_CLIENT_SOURCE_DIR}/tests/Update.cpp")
    target_link_libraries(mu_update_test PRIVATE mu_patch_core)
    if(MSVC)
        target_compile_options(mu_update_test PRIVATE /utf-8)
    endif()
    add_test(NAME update COMMAND mu_update_test)
endif()
