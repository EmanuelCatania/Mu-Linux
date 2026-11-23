#pragma once

// Edición conservadora de un .ini (Config.ini) o de un .toml plano
// (launcher.toml): sólo reescribe las claves que cambian.  Conserva los
// comentarios, el orden, las claves que no conoce (de una versión más nueva o
// de un mod), las mayúsculas de cada clave y los finales de línea.
//
// Como GetPrivateProfileString: secciones y claves sin distinguir mayúsculas,
// el valor sin espacios alrededor, y con una clave repetida vale la primera.
// Las claves antes de la primera sección van en la sección "" (el .toml).

#include <string>
#include <vector>

namespace Launcher {

class IniDocument
{
public:
	// " = " para el .toml, "=" para Config.ini: cómo se escriben las claves nuevas.
	explicit IniDocument(std::string separator = "=");

	// Un archivo que falta queda vacío (no es error).
	bool Load(const std::string& path);
	void Parse(const std::string& text);
	std::string Text() const;
	bool Save(const std::string& path) const;   // atómico

	bool Get(const std::string& section, const std::string& key, std::string& value) const;
	// Cambia la primera aparición de la clave; si no está, la agrega al final de
	// su sección (o crea la sección al final).  true si el texto cambió.
	bool Set(const std::string& section, const std::string& key, const std::string& value);

private:
	struct Line
	{
		std::string text;
		std::string section;     // sección a la que pertenece
		bool isKey = false;
		std::string key;
		std::size_t valueStart = 0, valueEnd = 0;   // tramo del valor en text
	};
	int Find(const std::string& section, const std::string& key) const;

	std::vector<Line> m_Lines;
	std::string m_Eol = "\n";
	bool m_FinalEol = true;
	std::string m_Separator;
};

} // namespace Launcher
