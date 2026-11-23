# Núcleo del actualizador (manifiesto firmado, zips, deltas, índice local, plan
# y descargas): lo comparten mu-patch, el launcher y sus pruebas.  No depende de
# SDL ni del cliente, así que tools/mu-patch lo puede compilar solo (por
# ejemplo, en el contenedor del server).
#
# Uso: set(MU_CLIENT_SOURCE_DIR <client>) + include(MuUpdate).  Define:
#   mu_update_core  biblioteca estática (incluir como "update/...").

if(TARGET mu_update_core)
    return()
endif()

include(FetchContent)
set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mu_zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR build/cmake)
FetchContent_MakeAvailable(mu_zstd)

# zlib sólo para Deflate (los zips que arma el admin con cualquier herramienta).
# Se compilan sus fuentes acá (SOURCE_SUBDIR inexistente), sin su CMake.
FetchContent_Declare(mu_zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.gz
    URL_HASH SHA256=bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16
    SOURCE_SUBDIR mu-sin-cmake)
FetchContent_MakeAvailable(mu_zlib)
add_library(mu_zlib STATIC
    "${mu_zlib_SOURCE_DIR}/adler32.c" "${mu_zlib_SOURCE_DIR}/crc32.c" "${mu_zlib_SOURCE_DIR}/deflate.c"
    "${mu_zlib_SOURCE_DIR}/inffast.c" "${mu_zlib_SOURCE_DIR}/inflate.c" "${mu_zlib_SOURCE_DIR}/inftrees.c"
    "${mu_zlib_SOURCE_DIR}/trees.c" "${mu_zlib_SOURCE_DIR}/zutil.c")
target_include_directories(mu_zlib PUBLIC "${mu_zlib_SOURCE_DIR}")
if(MSVC)
    target_compile_definitions(mu_zlib PRIVATE _CRT_SECURE_NO_DEPRECATE _CRT_NONSTDC_NO_DEPRECATE)
endif()

set(_mu_update_dir "${MU_CLIENT_SOURCE_DIR}/src/update")
add_library(mu_update_core STATIC
    "${_mu_update_dir}/Json.cpp"
    "${_mu_update_dir}/Json.h"
    "${_mu_update_dir}/Hash.cpp"
    "${_mu_update_dir}/Hash.h"
    "${_mu_update_dir}/Zip.cpp"
    "${_mu_update_dir}/Zip.h"
    "${_mu_update_dir}/Delta.cpp"
    "${_mu_update_dir}/Delta.h"
    "${_mu_update_dir}/Manifest.cpp"
    "${_mu_update_dir}/Manifest.h"
    "${_mu_update_dir}/Signing.cpp"
    "${_mu_update_dir}/Signing.h"
    "${_mu_update_dir}/FileUtil.cpp"
    "${_mu_update_dir}/FileUtil.h"
    "${_mu_update_dir}/InstallIndex.cpp"
    "${_mu_update_dir}/InstallIndex.h"
    "${_mu_update_dir}/InstallPlan.cpp"
    "${_mu_update_dir}/InstallPlan.h"
    "${_mu_update_dir}/Transfer.cpp"
    "${_mu_update_dir}/Transfer.h"
    "${_mu_update_dir}/Installer.cpp"
    "${_mu_update_dir}/Installer.h"
    "${_mu_update_dir}/ChannelFetch.cpp"
    "${_mu_update_dir}/ChannelFetch.h"
    "${MU_CLIENT_SOURCE_DIR}/src/net/crypto/monocypher.c")
target_include_directories(mu_update_core PUBLIC "${MU_CLIENT_SOURCE_DIR}/src"
    PRIVATE "${mu_zstd_SOURCE_DIR}/lib")
target_compile_features(mu_update_core PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(mu_update_core PUBLIC Threads::Threads PRIVATE libzstd_static mu_zlib)
if(MSVC)
    target_compile_options(mu_update_core PRIVATE /utf-8)
endif()
