#pragma once

// Lo que el launcher lleva adentro (lo genera cmake/MuEmbedFile.cmake): las
// imágenes del tema y la fuente.  Es un solo ejecutable, que se actualiza a sí
// mismo, así que no depende de archivos sueltos.

#include <cstddef>
#include <string>

namespace LauncherRes {

// Una imagen del tema compilado por su nombre de archivo sin extensión
// ("Button_n").  false si el tema no la trae.
bool FindImage(const std::string& name, const unsigned char** data, std::size_t* size);

extern const unsigned char FontRegular[];
extern const std::size_t FontRegularSize;
extern const unsigned char FontBold[];
extern const std::size_t FontBoldSize;

} // namespace LauncherRes
