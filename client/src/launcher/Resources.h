#pragma once

// Lo que el launcher lleva adentro (lo genera cmake/MuEmbedFile.cmake): las
// imágenes del tema y la fuente.  Es un solo ejecutable, que se actualiza a sí
// mismo, así que no depende de archivos sueltos.

#include <cstddef>

namespace LauncherRes {

extern const unsigned char MainBackground[];
extern const std::size_t MainBackgroundSize;
extern const unsigned char OptionsBackground[];
extern const std::size_t OptionsBackgroundSize;
extern const unsigned char MainImage[];
extern const std::size_t MainImageSize;
extern const unsigned char ButtonNormal[];
extern const std::size_t ButtonNormalSize;
extern const unsigned char ButtonHover[];
extern const std::size_t ButtonHoverSize;
extern const unsigned char ButtonDisabled[];
extern const std::size_t ButtonDisabledSize;
extern const unsigned char FontRegular[];
extern const std::size_t FontRegularSize;
extern const unsigned char FontBold[];
extern const std::size_t FontBoldSize;

} // namespace LauncherRes
