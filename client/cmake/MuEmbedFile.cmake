# Convierte un archivo en un arreglo de C++ (lo usa el launcher para llevar
# adentro las imágenes del tema y la fuente: es un solo ejecutable que se
# actualiza a sí mismo).
#
#   cmake -DINPUT=<archivo> -DOUTPUT=<.cpp> -DSYMBOL=<nombre> -P MuEmbedFile.cmake
#
# Define en el namespace LauncherRes:
#   extern const unsigned char <SYMBOL>[];  extern const std::size_t <SYMBOL>Size;

file(READ "${INPUT}" _hex HEX)
string(LENGTH "${_hex}" _len)
math(EXPR _size "${_len} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
# Una línea cada 32 bytes, para que el archivo se pueda abrir.
string(REGEX REPLACE "((0x[0-9a-f][0-9a-f],){32})" "\\1\n" _bytes "${_bytes}")
file(WRITE "${OUTPUT}.tmp"
"// Generado por cmake/MuEmbedFile.cmake desde ${INPUT}.  No editar.\n"
"#include <cstddef>\n"
"namespace LauncherRes {\n"
"extern const unsigned char ${SYMBOL}[] = {\n${_bytes}\n};\n"
"extern const std::size_t ${SYMBOL}Size = ${_size};\n"
"}\n")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
