# Launcher (Fase 5): ventana SDL3 con el tema WebzenOldSchoolV1 de kayito, que
# actualiza el cliente (núcleo en src/update), edita sus opciones (Config.ini,
# con la tabla compartida app/config/OptionTable.h) y lo arranca.
#
# Se compila solo, como mu-patch (no necesita el resto del cliente):
#   cmake -S client/src/launcher -B build/launcher -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/launcher --config Release
#   ctest --test-dir build/launcher -C Release
# En Linux hacen falta OpenSSL de la distribución (libssl-dev) y lo que pide SDL3
# (Wayland, X11, libdecor: ver cmake/MuSDL3.cmake).
#
# Lo que fija el operador (ver LauncherConfig.h), por ejemplo:
#   -DMU_LAUNCHER_TITLE="Mi Server" -DMU_BASE_KEY=<64 hex>
#   -DMU_OVERLAY_URL=https://miserver.com/updates -DMU_OVERLAY_CHANNEL=miserver/overlay
#   -DMU_OVERLAY_KEY=<64 hex> -DMU_CONNECT_SERVER=mu.miserver.com:44405
# Con MU_CONNECT_SERVER y MU_OVERLAY_KEY alcanza: lo demás lo anuncia el server
# (launcher.json, ver ServerAnnounce.h).

cmake_minimum_required(VERSION 3.25)
project(mu_launcher LANGUAGES C CXX)

if(NOT DEFINED MU_CLIENT_SOURCE_DIR)
    get_filename_component(MU_CLIENT_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
endif()
list(APPEND CMAKE_MODULE_PATH "${MU_CLIENT_SOURCE_DIR}/cmake")
include(MuUpdateHttp)
include(MuSDL3)

set(MU_LAUNCHER_TITLE "" CACHE STRING "Título de la ventana del launcher")
set(MU_BASE_REPO "" CACHE STRING "Repo de GitHub con los Releases de la base (dueño/repo)")
set(MU_BASE_TAG "" CACHE STRING "Tag de la base (vacío: MU_RELEASE del cliente)")
set(MU_BASE_KEY "" CACHE STRING "Clave pública que firma los Releases de la base (64 hex)")
set(MU_OVERLAY_URL "" CACHE STRING "Carpeta HTTP con los parches del server")
set(MU_OVERLAY_CHANNEL "" CACHE STRING "Canal de los parches del server (miserver/overlay)")
set(MU_OVERLAY_KEY "" CACHE STRING "Identidad del server (64 hex)")
set(MU_CONNECT_SERVER "" CACHE STRING "ConnectServer del server (host:puerto, varios separados por coma)")
set(MU_GAME_EXE "" CACHE STRING "Ejecutable del cliente (vacío: main.exe / main)")

# Imágenes del tema y fuente, adentro del ejecutable.
set(_theme "${CMAKE_CURRENT_SOURCE_DIR}/themes/WebzenOldSchoolV1")
set(_font "${MU_CLIENT_SOURCE_DIR}/bin/Client/System/Font")
set(_embedded)
foreach(_pair
        "MainBackground=${_theme}/MainBackground.png"
        "OptionsBackground=${_theme}/OptionsBackground.png"
        "MainImage=${_theme}/MainImage.png"
        "ButtonNormal=${_theme}/Button_n.png"
        "ButtonHover=${_theme}/Button_h.png"
        "ButtonDisabled=${_theme}/Button_d.png"
        "FontRegular=${_font}/LiberationSans-Regular.ttf"
        "FontBold=${_font}/LiberationSans-Bold.ttf")
    string(FIND "${_pair}" "=" _eq)
    string(SUBSTRING "${_pair}" 0 ${_eq} _sym)
    math(EXPR _from "${_eq} + 1")
    string(SUBSTRING "${_pair}" ${_from} -1 _file)
    set(_out "${CMAKE_CURRENT_BINARY_DIR}/res/${_sym}.cpp")
    add_custom_command(OUTPUT "${_out}"
        COMMAND "${CMAKE_COMMAND}" -DINPUT=${_file} -DOUTPUT=${_out} -DSYMBOL=${_sym}
                -P "${MU_CLIENT_SOURCE_DIR}/cmake/MuEmbedFile.cmake"
        DEPENDS "${_file}" "${MU_CLIENT_SOURCE_DIR}/cmake/MuEmbedFile.cmake"
        COMMENT "Embebiendo ${_sym}"
        VERBATIM)
    list(APPEND _embedded "${_out}")
endforeach()

# Lo que no depende de la ventana: configuración y flujo de actualización (lo
# usan el ejecutable y las pruebas).
add_library(mu_launcher_core STATIC
    IniDocument.cpp IniDocument.h
    LauncherConfig.cpp LauncherConfig.h
    OptionsModel.cpp OptionsModel.h
    SelfUpdate.cpp SelfUpdate.h
    CsClient.cpp CsClient.h
    ServerAnnounce.cpp ServerAnnounce.h
    UpdateFlow.cpp UpdateFlow.h
    # El canal cifrado y la confianza en la identidad, los mismos del cliente.
    "${MU_CLIENT_SOURCE_DIR}/src/net/SecureChannel.cpp"
    "${MU_CLIENT_SOURCE_DIR}/src/net/ServerTrustCore.cpp")
target_include_directories(mu_launcher_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${MU_CLIENT_SOURCE_DIR}/src")
target_link_libraries(mu_launcher_core PUBLIC mu_update_http)
if(WIN32)
    target_link_libraries(mu_launcher_core PUBLIC ws2_32)
endif()

add_executable(launcher WIN32
    main.cpp
    Gfx.cpp
    Gfx.h
    Resources.h
    ${_embedded})
if(WIN32)
    target_sources(launcher PRIVATE launcher.rc)
endif()
target_include_directories(launcher PRIVATE "${MU_CLIENT_SOURCE_DIR}/lib")
target_link_libraries(launcher PRIVATE mu_launcher_core SDL3::SDL3-static)
foreach(_opt MU_LAUNCHER_TITLE MU_BASE_REPO MU_BASE_TAG MU_BASE_KEY MU_OVERLAY_URL MU_OVERLAY_CHANNEL MU_OVERLAY_KEY MU_CONNECT_SERVER MU_GAME_EXE)
    if(NOT "${${_opt}}" STREQUAL "")
        target_compile_definitions(mu_launcher_core PRIVATE "${_opt}=\"${${_opt}}\"")
    endif()
endforeach()
if(MSVC)
    target_compile_options(launcher PRIVATE /utf-8)
    target_compile_options(mu_launcher_core PRIVATE /utf-8)
endif()

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()
    # La prueba arma canales con mu-patch.
    add_subdirectory("${MU_CLIENT_SOURCE_DIR}/tools/mu-patch" mu-patch EXCLUDE_FROM_ALL)
    add_executable(mu_update_http_test "${MU_CLIENT_SOURCE_DIR}/tests/UpdateHttp.cpp")
    target_link_libraries(mu_update_http_test PRIVATE mu_launcher_core mu_patch_core)
    if(WIN32)
        target_link_libraries(mu_update_http_test PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(mu_update_http_test PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME update_http COMMAND mu_update_http_test)
endif()
