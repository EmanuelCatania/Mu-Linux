#pragma once

// Pestañas de opciones del launcher: qué opciones hay, sus valores posibles y
// cómo se leen y guardan.  Sin ventana, para poder probarlo.
//
// Las opciones del juego van a User/Config.ini (el mismo que lee el cliente, con
// la tabla compartida app/config/OptionTable.h); las de descarga, a
// User/launcher.toml.  Guardar sólo reescribe lo que el jugador cambió.  Un
// valor guardado fuera de rango se muestra como el default y no se toca hasta
// que el jugador lo cambie.

#include "IniDocument.h"

#include <string>
#include <vector>

namespace Launcher {

enum class OptionFile { Game, Launcher };

struct OptionChoice
{
	std::string label;
	std::string value;        // lo que se escribe
};

struct OptionRow
{
	std::string label;
	OptionFile file = OptionFile::Game;
	std::string section;
	std::string key;
	std::vector<OptionChoice> choices;
	int defaultIndex = 0;
	bool numeric = true;      // compara por número ("01" == "1"); si no, por texto sin mayúsculas
	// Fila de texto libre (choices vacío): se escribe tal cual, hasta maxLength
	// caracteres de los que acepta AllowedTextChar.
	bool text = false;
	std::size_t maxLength = 0;
};

// Caracteres aceptados en una fila de texto (letras y números ASCII, como las
// cuentas del server).
bool AllowedTextChar(char c);

struct OptionTab
{
	std::string name;
	std::vector<OptionRow> rows;
};

std::vector<OptionTab> BuildOptionTabs();

class OptionsEditor
{
public:
	OptionsEditor();

	// configIni: User/Config.ini; si falta y existe la plantilla, se parte de ella.
	bool Load(const std::string& configIni, const std::string& templateIni, const std::string& launcherToml);

	const std::vector<OptionTab>& Tabs() const { return m_Tabs; }
	int Current(std::size_t tab, std::size_t row) const;
	void Cycle(std::size_t tab, std::size_t row, int direction);
	// Con el juego abierto las opciones del juego (Config.ini) sólo se ven: el
	// juego también lo escribe desde su menú.  Las del launcher siguen editables.
	void SetGameLocked(bool locked) { m_GameLocked = locked; }
	bool GameLocked() const { return m_GameLocked; }
	bool Editable(std::size_t tab, std::size_t row) const;
	// Filas de texto.  SetText descarta los caracteres no aceptados y corta en
	// maxLength.
	std::string Text(std::size_t tab, std::size_t row) const;
	void SetText(std::size_t tab, std::size_t row, const std::string& value);
	void Select(std::size_t tab, std::size_t row, int index);
	bool Dirty() const;
	// Escribe sólo lo cambiado.  *downloadsChanged: cambió algo que afecta qué
	// se baja (música, HD, conexiones, límite).
	bool Save(bool* downloadsChanged);
	void Revert();

	// Para las pruebas.
	const IniDocument& Game() const { return m_Game; }
	const IniDocument& LauncherToml() const { return m_Toml; }

private:
	int Stored(const OptionRow& r) const;
	std::string StoredText(const OptionRow& r) const;

	std::vector<OptionTab> m_Tabs;
	std::vector<std::vector<int>> m_Selected;   // -1: sin cambios (en las de texto, 1: editada)
	std::vector<std::vector<std::string>> m_Text;
	bool m_GameLocked = false;
	IniDocument m_Game;
	IniDocument m_Toml;
	std::string m_GamePath;
	std::string m_TomlPath;
};

} // namespace Launcher
