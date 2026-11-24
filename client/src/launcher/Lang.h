#pragma once

// Idioma del launcher: el mismo que el del juego ([Language] LangSelection de
// Config.ini).  Los textos se escriben en castellano en el código y se buscan
// en la tabla de Lang.cpp; el que no está sale en castellano.
//
// Fmt reemplaza {0}, {1}... por los argumentos, así cada idioma los ubica
// donde le quede bien.

#include <initializer_list>
#include <string>

namespace Launcher {

enum class Language { Spanish, English, Portuguese };

// "Eng" -> inglés, "Por" -> portugués; "Spn", vacío ("Original") o cualquier
// otro -> castellano, que es el idioma del cliente original.
Language LanguageFromSetting(const std::string& langSelection);

// Se puede cambiar con el flujo de actualización corriendo: los mensajes que
// arme de ahí en adelante salen en el idioma nuevo.
void SetLanguage(Language language);
Language CurrentLanguage();

const char* Tr(const char* es);
std::string Tr(const std::string& es);
std::string Fmt(const char* es, std::initializer_list<std::string> args);

// Para las pruebas: si la tabla tiene una traducción para ese texto.
bool HasTranslation(const std::string& es);

} // namespace Launcher
