# Launcher (Fase 5): ventana SDL3 con uno de los temas de kayito, que
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
#   -DMU_LAUNCHER_THEME=WebzenOldSchoolV2   (una carpeta de themes/)
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
set(MU_WEB_URL "" CACHE STRING "Sitio del server (si no lo anuncia el server)")
set(MU_GAME_EXE "" CACHE STRING "Ejecutable del cliente (vacío: main.exe / main)")
set(MU_LAUNCHER_THEME "WebzenOldSchoolV1" CACHE STRING "Tema del launcher (una carpeta de themes/)")
# La web del server en el panel de la imagen (WebPanel.h).  Sólo Windows con
# MSVC: el SDK de WebView2 sale de NuGet y su loader se enlaza estático.
if(WIN32 AND MSVC)
    option(MU_LAUNCHER_WEBVIEW "Mostrar la web del server en el launcher (WebView2)" ON)
else()
    set(MU_LAUNCHER_WEBVIEW OFF)
endif()

# El tema: su Theme.cpp (disposición y colores, ver ThemeSpec.h), su ícono y
# todas sus imágenes, adentro del ejecutable con la fuente.
set(_theme "${CMAKE_CURRENT_SOURCE_DIR}/themes/${MU_LAUNCHER_THEME}")
if(NOT EXISTS "${_theme}/Theme.cpp")
    file(GLOB _themes RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/themes" "${CMAKE_CURRENT_SOURCE_DIR}/themes/*/Theme.cpp")
    string(REPLACE "/Theme.cpp" "" _themes "${_themes}")
    message(FATAL_ERROR "MU_LAUNCHER_THEME=${MU_LAUNCHER_THEME} no es un tema. Los que hay: ${_themes}")
endif()
set(_font "${MU_CLIENT_SOURCE_DIR}/bin/Client/System/Font")
file(GLOB _theme_images CONFIGURE_DEPENDS "${_theme}/*.png")
set(_pairs "FontRegular=${_font}/LiberationSans-Regular.ttf" "FontBold=${_font}/LiberationSans-Bold.ttf")
set(_index_decl "")
set(_index_rows "")
foreach(_img IN LISTS _theme_images)
    get_filename_component(_base "${_img}" NAME_WE)
    string(MAKE_C_IDENTIFIER "Theme_${_base}" _sym)
    list(APPEND _pairs "${_sym}=${_img}")
    string(APPEND _index_decl "extern const unsigned char ${_sym}[];\nextern const std::size_t ${_sym}Size;\n")
    string(APPEND _index_rows "\t{ \"${_base}\", ${_sym}, &${_sym}Size },\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/res/ThemeIndex.cpp" CONTENT
"// Generado por client/src/launcher/CMakeLists.txt (tema ${MU_LAUNCHER_THEME}).  No editar.
#include \"Resources.h\"
#include <cstring>
namespace LauncherRes {
${_index_decl}
namespace {
struct Row { const char* name; const unsigned char* data; const std::size_t* size; };
const Row kRows[] = {
${_index_rows}\t{ nullptr, nullptr, nullptr }
};
}
bool FindImage(const std::string& name, const unsigned char** data, std::size_t* size)
{
\tfor (const Row* r = kRows; r->name; ++r)
\t\tif (name == r->name) { *data = r->data; *size = *r->size; return true; }
\treturn false;
}
}
" @ONLY)
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/res/launcher.rc" CONTENT "IDI_ICON1 ICON \"${_theme}/Icon.ico\"\n" @ONLY)
# Lo generado a partir de los archivos del tema va en una carpeta y con un
# nombre por tema.  Todos los temas tienen un Theme.cpp y casi todos las mismas
# imágenes (MainBackground.png...): con la misma salida, al cambiar de tema en
# la misma carpeta de build MSBuild compara fechas, ve el .obj o el .cpp
# generado más nuevo que el archivo del tema nuevo y se queda con el del tema
# anterior.
set(_theme_out "${CMAKE_CURRENT_BINARY_DIR}/res/${MU_LAUNCHER_THEME}")
set(_theme_src "${_theme_out}/Theme_${MU_LAUNCHER_THEME}.cpp")
file(CONFIGURE OUTPUT "${_theme_src}" CONTENT "#include \"${_theme}/Theme.cpp\"\n" @ONLY)
set(_embedded "${CMAKE_CURRENT_BINARY_DIR}/res/ThemeIndex.cpp")
foreach(_pair IN LISTS _pairs)
    string(FIND "${_pair}" "=" _eq)
    string(SUBSTRING "${_pair}" 0 ${_eq} _sym)
    math(EXPR _from "${_eq} + 1")
    string(SUBSTRING "${_pair}" ${_from} -1 _file)
    set(_out "${_theme_out}/${_sym}.cpp")
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
    Lang.cpp Lang.h
    LauncherConfig.cpp LauncherConfig.h
    OptionsModel.cpp OptionsModel.h
    SelfUpdate.cpp SelfUpdate.h
    CsClient.cpp CsClient.h
    ServerAnnounce.cpp ServerAnnounce.h
    UpdateFlow.cpp UpdateFlow.h
    # El canal cifrado y la confianza en la identidad, los mismos del cliente.
    "${MU_CLIENT_SOURCE_DIR}/src/net/SecureChannel.cpp"
    "${MU_CLIENT_SOURCE_DIR}/src/net/ServerTrustCore.cpp"
    # "Hay un cliente corriendo": el lock que toma el juego.
    "${MU_CLIENT_SOURCE_DIR}/src/platform/RunningLock.cpp")
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
    ThemeSpec.h
    WebPanel.cpp
    WebPanel.h
    "${_theme_src}"
    ${_embedded})
if(WIN32)
    target_sources(launcher PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/res/launcher.rc")
endif()
target_include_directories(launcher PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${MU_CLIENT_SOURCE_DIR}/lib")
target_link_libraries(launcher PRIVATE mu_launcher_core SDL3::SDL3-static)
if(MU_LAUNCHER_WEBVIEW)
    include(FetchContent)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_wv2_arch x64)
    else()
        set(_wv2_arch x86)
    endif()
    FetchContent_Declare(mu_webview2
        URL https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4314.35
        URL_HASH SHA256=90778deb1b77e36292828b4b5833c9255c27533f40e0ddd55891ae718944edc4
        DOWNLOAD_NAME webview2.zip)
    FetchContent_MakeAvailable(mu_webview2)
    target_include_directories(launcher PRIVATE "${mu_webview2_SOURCE_DIR}/build/native/include")
    target_link_libraries(launcher PRIVATE "${mu_webview2_SOURCE_DIR}/build/native/${_wv2_arch}/WebView2LoaderStatic.lib" ole32 shlwapi version)
    target_compile_definitions(launcher PRIVATE MU_WEBVIEW2)
endif()
foreach(_opt MU_LAUNCHER_TITLE MU_BASE_REPO MU_BASE_TAG MU_BASE_KEY MU_OVERLAY_URL MU_OVERLAY_CHANNEL MU_OVERLAY_KEY MU_CONNECT_SERVER MU_WEB_URL MU_GAME_EXE)
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
    # La prueba del idioma lee las fuentes del launcher.
    target_compile_definitions(mu_update_http_test PRIVATE MU_LAUNCHER_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    if(WIN32)
        target_link_libraries(mu_update_http_test PRIVATE ws2_32)
    endif()
    if(MSVC)
        target_compile_options(mu_update_http_test PRIVATE /utf-8 /EHsc)
    endif()
    add_test(NAME update_http COMMAND mu_update_http_test)
endif()
