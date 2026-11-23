# Launcher (Fase 5): por ahora, el transporte HTTP del actualizador y su prueba.
# La ventana SDL3 viene en el paso siguiente.
#
# Se compila solo, como mu-patch (no necesita el resto del cliente):
#   cmake -S client/src/launcher -B build/launcher -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/launcher --config Release
#   ctest --test-dir build/launcher -C Release
# En Linux hace falta OpenSSL de la distribución (libssl-dev).

cmake_minimum_required(VERSION 3.25)
project(mu_launcher LANGUAGES C CXX)

if(NOT DEFINED MU_CLIENT_SOURCE_DIR)
    get_filename_component(MU_CLIENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
list(APPEND CMAKE_MODULE_PATH "${MU_CLIENT_SOURCE_DIR}/cmake")
include(MuUpdateHttp)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    # La prueba arma canales con mu-patch.
    add_subdirectory("${MU_CLIENT_SOURCE_DIR}/tools/mu-patch" mu-patch EXCLUDE_FROM_ALL)
    add_executable(mu_update_http_test "${MU_CLIENT_SOURCE_DIR}/tests/UpdateHttp.cpp")
    target_link_libraries(mu_update_http_test PRIVATE mu_update_http mu_patch_core)
    if(WIN32)
        target_link_libraries(mu_update_http_test PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(mu_update_http_test PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME update_http COMMAND mu_update_http_test)
endif()
