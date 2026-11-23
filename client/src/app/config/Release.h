#pragma once

// Versión del cliente: la del tag de la Release desde la que se compiló.
//
// La usa el launcher para saber qué base bajar: mu-patch la lee del ejecutable
// (busca la marca "MuRelease1:<versión>") y la escribe en requires.base del
// overlay del server, así el admin no la tipea.
//
// NO es ServerConfig::ClientVersion: ésa es la que el server compara en el
// login y no cambia con cada Release (vale 0.97.40 desde la 0.97.40).
//
// Se actualiza en cada Release, junto con las demás versiones del repo.  Un build
// de CMake la puede fijar con -DMU_RELEASE=<versión>.

#ifndef MU_RELEASE
#define MU_RELEASE "0.97.46"
#endif

namespace ClientRelease {

// "MuRelease1:" MU_RELEASE.  Definida en Config_Load.cpp, que la escribe en el
// log al arrancar: así el linker no la descarta.
extern const char kMarker[];

constexpr unsigned kPrefixLength = 11;   // strlen("MuRelease1:")

inline const char* Version() { return kMarker + kPrefixLength; }

} // namespace ClientRelease
