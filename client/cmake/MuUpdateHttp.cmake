# Transporte HTTP del actualizador (libcurl).  Aparte de mu_update_core para que
# mu-patch y el contenedor del server no necesiten libcurl.
#
# libcurl se compila desde fuentes, sólo con HTTP y HTTPS, y con el TLS del
# sistema: Schannel en Windows (sin dependencias) y OpenSSL en Linux (el de la
# distribución, con sus certificados).
#
# Uso: set(MU_CLIENT_SOURCE_DIR <client>) + include(MuUpdateHttp).  Define:
#   mu_update_http  biblioteca estática (incluir como "update/HttpTransport.h").

if(TARGET mu_update_http)
    return()
endif()

include(MuUpdate)
include(FetchContent)

set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(BUILD_LIBCURL_DOCS OFF CACHE BOOL "" FORCE)
set(BUILD_MISC_DOCS OFF CACHE BOOL "" FORCE)
set(ENABLE_CURL_MANUAL OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(HTTP_ONLY ON CACHE BOOL "" FORCE)
set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
set(CURL_USE_LIBSSH OFF CACHE BOOL "" FORCE)
set(CURL_USE_GSSAPI OFF CACHE BOOL "" FORCE)
set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
set(USE_NGTCP2 OFF CACHE BOOL "" FORCE)
set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
set(USE_WIN32_IDN OFF CACHE BOOL "" FORCE)
set(USE_LIBRTMP OFF CACHE BOOL "" FORCE)
set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
set(CURL_USE_PKGCONFIG OFF CACHE BOOL "" FORCE)
set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
if(WIN32)
    set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
else()
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    # Los certificados del sistema donde corre, no los de la máquina que compiló.
    set(CURL_CA_BUNDLE "none" CACHE STRING "" FORCE)
    set(CURL_CA_PATH "none" CACHE STRING "" FORCE)
    set(CURL_CA_FALLBACK ON CACHE BOOL "" FORCE)
endif()
# Las pruebas de curl no se compilan (BUILD_TESTING es global; lo restauramos).
set(_mu_build_testing "${BUILD_TESTING}")
set(BUILD_TESTING OFF)
FetchContent_Declare(mu_curl
    URL https://github.com/curl/curl/releases/download/curl-8_23_0/curl-8.23.0.tar.xz
    URL_HASH SHA256=8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5)
FetchContent_MakeAvailable(mu_curl)
set(BUILD_TESTING "${_mu_build_testing}")

add_library(mu_update_http STATIC
    "${MU_CLIENT_SOURCE_DIR}/src/update/HttpTransport.cpp"
    "${MU_CLIENT_SOURCE_DIR}/src/update/HttpTransport.h")
target_link_libraries(mu_update_http PUBLIC mu_update_core PRIVATE CURL::libcurl)
target_compile_features(mu_update_http PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(mu_update_http PRIVATE /utf-8)
endif()
