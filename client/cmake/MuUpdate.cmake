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
    "${MU_CLIENT_SOURCE_DIR}/src/net/crypto/monocypher.c")
target_include_directories(mu_update_core PUBLIC "${MU_CLIENT_SOURCE_DIR}/src"
    PRIVATE "${mu_zstd_SOURCE_DIR}/lib")
target_compile_features(mu_update_core PUBLIC cxx_std_17)
find_package(Threads REQUIRED)
target_link_libraries(mu_update_core PUBLIC Threads::Threads PRIVATE libzstd_static)
if(MSVC)
    target_compile_options(mu_update_core PRIVATE /utf-8)
endif()
