#include "OptionsModel.h"

#include "app/config/OptionTable.h"
#include "update/FileUtil.h"

#include <cctype>
#include <cstdlib>

namespace Launcher {

namespace Opt = UserOptionTable;

namespace {

OptionRow Toggle(const char* label, const Opt::IntOption& o)
{
	OptionRow r;
	r.label = label;
	r.section = o.section;
	r.key = o.key;
	r.choices = { { "No", "0" }, { "Sí", "1" } };
	r.defaultIndex = o.def;
	return r;
}

// Opción con un texto por valor (labels[i] corresponde a o.min + i).
OptionRow Levels(const char* label, const Opt::IntOption& o, std::vector<const char*> labels)
{
	OptionRow r;
	r.label = label;
	r.section = o.section;
	r.key = o.key;
	for (int v = o.min; v <= o.max; ++v)
	{
		const std::size_t i = static_cast<std::size_t>(v - o.min);
		r.choices.push_back({ i < labels.size() ? labels[i] : std::to_string(v), std::to_string(v) });
	}
	r.defaultIndex = o.def - o.min;
	return r;
}

OptionRow Numbers(const char* label, const Opt::IntOption& o)
{
	return Levels(label, o, {});
}

OptionRow Values(const char* label, const Opt::IntOption& o, std::vector<std::pair<int, const char*>> values)
{
	OptionRow r;
	r.label = label;
	r.section = o.section;
	r.key = o.key;
	for (const auto& [v, l] : values)
	{
		if (v == o.def) r.defaultIndex = static_cast<int>(r.choices.size());
		r.choices.push_back({ l, std::to_string(v) });
	}
	return r;
}

OptionRow LauncherRow(const char* label, const char* key, std::vector<OptionChoice> choices, int def, bool numeric = true)
{
	OptionRow r;
	r.label = label;
	r.file = OptionFile::Launcher;
	r.key = key;
	r.choices = std::move(choices);
	r.defaultIndex = def;
	r.numeric = numeric;
	return r;
}

bool EqualNoCase(const std::string& a, const std::string& b)
{
	if (a.size() != b.size()) return false;
	for (std::size_t i = 0; i < a.size(); ++i)
		if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
	return true;
}

bool ParseInt(const std::string& s, long& out)
{
	if (s.empty()) return false;
	char* end = nullptr;
	out = std::strtol(s.c_str(), &end, 10);
	return end && *end == 0;
}

} // namespace

bool AllowedTextChar(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

std::vector<OptionTab> BuildOptionTabs()
{
	std::vector<OptionTab> tabs;

	OptionTab general{ "General", {} };
	{
		// El cliente lo pone en el campo de usuario del login (CUserSettings).
		OptionRow r;
		r.label = "Usuario guardado";
		r.section = "User";
		r.key = "Username";
		r.text = true;
		r.maxLength = 10;
		general.rows.push_back(r);
	}
	{
		OptionRow r;
		// También el del launcher.
		r.label = "Idioma";
		r.section = "Language";
		r.key = "LangSelection";
		r.numeric = false;
		r.choices = { { "Original (0.97k)", "" }, { "English", Opt::kLanguageSuffixes[0] },
		              { "Español", Opt::kLanguageSuffixes[1] }, { "Português", Opt::kLanguageSuffixes[2] } };
		general.rows.push_back(r);
	}
	general.rows.push_back(LauncherRow("Tamaño del launcher", "Scale",
	                                   { { "Automático", "0" }, { "100%", "100" }, { "125%", "125" }, { "150%", "150" },
	                                     { "175%", "175" }, { "200%", "200" } }, 0));
	tabs.push_back(general);

	OptionTab screen{ "Pantalla", {} };
	screen.rows.push_back(Toggle("Modo ventana", Opt::kWindowMode));
	screen.rows.push_back(Toggle("Sin bordes", Opt::kBorderless));
	{
		OptionRow r;
		r.label = "Resolución";
		r.section = Opt::kResolution.section;
		r.key = Opt::kResolution.key;
		for (int i = 0; i < Opt::kResolutionCount; ++i)
			r.choices.push_back({ std::to_string(Opt::kResolutions[i].width) + "x" + std::to_string(Opt::kResolutions[i].height), std::to_string(i) });
		r.defaultIndex = Opt::kResolution.def;
		screen.rows.push_back(r);
	}
	{
		OptionRow r;
		r.label = "Render";
		r.section = "Window";
		r.key = "RenderBackend";
		r.numeric = false;
		for (int i = 0; i < Opt::kRenderBackendCount; ++i)
		{
#if !defined(_WIN32) || defined(__MINGW32__)
			// DirectX sólo existe en Windows con MSVC (ver CUserSettings::IsRenderBackendAvailable).
			if (i == 2 || i == 3) continue;
#endif
			r.choices.push_back({ Opt::kRenderBackendNames[i], Opt::kRenderBackendKeys[i] });
		}
		screen.rows.push_back(r);
	}
	screen.rows.push_back(Toggle("Sincronización vertical", Opt::kVSync));
	screen.rows.push_back(Values("Límite de FPS", Opt::kFpsLimit,
	                             { { 0, "Sin límite" }, { 30, "30" }, { 60, "60" }, { 75, "75" }, { 120, "120" }, { 144, "144" }, { 165, "165" }, { 240, "240" } }));
	screen.rows.push_back(Toggle("Dibujo desacoplado", Opt::kRenderDecoupled));
	tabs.push_back(screen);

	OptionTab gfx{ "Gráficos", {} };
	gfx.rows.push_back(Levels("Calidad de texturas", Opt::kTextureQuality, { "Original", "Mipmaps", "Mipmaps + bicúbico" }));
	gfx.rows.push_back(Toggle("Texturas HD", Opt::kHDTextures));
	gfx.rows.push_back(Levels("Nitidez", Opt::kSharpness, { "Apagada", "Baja", "Media", "Alta" }));
	gfx.rows.push_back(Levels("Antialiasing", Opt::kAntialiasing, { "Apagado", "FXAA", "MSAA 4x", "MSAA 4x + FXAA" }));
	gfx.rows.push_back(Levels("Sombras", Opt::kShadows, { "Apagadas", "Suaves", "Fuertes" }));
	gfx.rows.push_back(Toggle("Niebla por profundidad", Opt::kDepthFog));
	gfx.rows.push_back(Levels("Distancia de dibujado", Opt::kDrawDistance, { "Normal", "x1,5", "x2", "x3" }));
	gfx.rows.push_back(Levels("Luces dinámicas", Opt::kDynamicLights, { "Apagadas", "Conservadoras", "Contrastadas" }));
	gfx.rows.push_back(Levels("Bloom", Opt::kBloom, { "Apagado", "Suave", "Fuerte" }));
	gfx.rows.push_back(Toggle("Cielo", Opt::kSky));
	gfx.rows.push_back(Levels("Bajo el agua", Opt::kUnderwater, { "Apagado", "Sin ondulación", "Completo" }));
	gfx.rows.push_back(Toggle("Agua mejorada", Opt::kWorldWater));
	tabs.push_back(gfx);

	OptionTab perf{ "Rendimiento", {} };
	static const char* const kAntilagLabels[Opt::kAntilagCount] = {
		"Quitar sombras", "Quitar objetos", "Quitar piso", "Quitar skills", "Quitar efectos fijos",
		"Quitar efectos móviles", "Quitar alas", "Quitar barras de vida", "Quitar interfaz",
		"Quitar clima", "Quitar brillo",
	};
	for (int i = 0; i < Opt::kAntilagCount; ++i)
	{
		const Opt::IntOption o{ "Antilag", Opt::kAntilagKeys[i], 0, 1, 0 };
		perf.rows.push_back(Toggle(kAntilagLabels[i], o));
	}
	tabs.push_back(perf);

	OptionTab sound{ "Sonido", {} };
	sound.rows.push_back(Toggle("Efectos de sonido", Opt::kEnableSound));
	sound.rows.push_back(Numbers("Volumen de efectos", Opt::kSoundLevel));
	sound.rows.push_back(Toggle("Música", Opt::kEnableMusic));
	sound.rows.push_back(Numbers("Volumen de la música", Opt::kMusicLevel));
	tabs.push_back(sound);

	OptionTab text{ "Texto", {} };
	text.rows.push_back(Toggle("Sombra del texto", Opt::kTextShadow));
	text.rows.push_back(Toggle("Contorno del texto", Opt::kTextOutline));
	text.rows.push_back(Toggle("Emoji en color", Opt::kColorEmoji));
	tabs.push_back(text);

	OptionTab map{ "Minimapa", {} };
	map.rows.push_back(Numbers("Opacidad", Opt::kMiniMapAlpha));
	map.rows.push_back(Numbers("Zoom", Opt::kMiniMapZoom));
	tabs.push_back(map);

	OptionTab dl{ "Descargas", {} };
	const std::vector<OptionChoice> yesNo = { { "No", "false" }, { "Sí", "true" } };
	dl.rows.push_back(LauncherRow("Bajar la música", "Music", yesNo, 1, false));
	dl.rows.push_back(LauncherRow("Bajar texturas HD", "HD", yesNo, 0, false));
	std::vector<OptionChoice> conns;
	for (int i = 1; i <= 8; ++i) conns.push_back({ std::to_string(i), std::to_string(i) });
	dl.rows.push_back(LauncherRow("Conexiones", "Connections", conns, 3));
	dl.rows.push_back(LauncherRow("Límite de velocidad", "SpeedLimitKB",
	                              { { "Sin límite", "0" }, { "256 KB/s", "256" }, { "512 KB/s", "512" }, { "1 MB/s", "1024" },
	                                { "2 MB/s", "2048" }, { "5 MB/s", "5120" }, { "10 MB/s", "10240" } }, 0));
	dl.rows.push_back(LauncherRow("Límite jugando", "PlayingSpeedLimitKB",
	                              { { "Sin límite", "0" }, { "128 KB/s", "128" }, { "256 KB/s", "256" }, { "512 KB/s", "512" },
	                                { "1 MB/s", "1024" }, { "2 MB/s", "2048" } }, 3));
	tabs.push_back(dl);
	return tabs;
}

OptionsEditor::OptionsEditor() : m_Tabs(BuildOptionTabs()), m_Game("="), m_Toml(" = ")
{
	Revert();
}

bool OptionsEditor::Load(const std::string& configIni, const std::string& templateIni, const std::string& launcherToml)
{
	m_GamePath = configIni;
	m_TomlPath = launcherToml;
	bool ok = true;
	if (!Update::FileExists(configIni) && !templateIni.empty() && Update::FileExists(templateIni))
		ok = m_Game.Load(templateIni);
	else
		ok = m_Game.Load(configIni);
	ok = m_Toml.Load(launcherToml) && ok;
	Revert();
	return ok;
}

void OptionsEditor::Revert()
{
	m_Selected.assign(m_Tabs.size(), {});
	m_Text.assign(m_Tabs.size(), {});
	for (std::size_t t = 0; t < m_Tabs.size(); ++t)
	{
		m_Selected[t].assign(m_Tabs[t].rows.size(), -1);
		m_Text[t].assign(m_Tabs[t].rows.size(), std::string());
	}
}

std::string OptionsEditor::StoredText(const OptionRow& r) const
{
	const IniDocument& doc = r.file == OptionFile::Game ? m_Game : m_Toml;
	std::string v;
	doc.Get(r.section, r.key, v);
	return v;
}

std::string OptionsEditor::Text(std::size_t tab, std::size_t row) const
{
	return m_Selected[tab][row] >= 0 ? m_Text[tab][row] : StoredText(m_Tabs[tab].rows[row]);
}

void OptionsEditor::SetText(std::size_t tab, std::size_t row, const std::string& value)
{
	const OptionRow& r = m_Tabs[tab].rows[row];
	if (!r.text || !Editable(tab, row)) return;
	std::string clean;
	for (char c : value)
		if (AllowedTextChar(c) && clean.size() < r.maxLength) clean.push_back(c);
	m_Text[tab][row] = clean;
	m_Selected[tab][row] = clean == StoredText(r) ? -1 : 1;
}

int OptionsEditor::Stored(const OptionRow& r) const
{
	const IniDocument& doc = r.file == OptionFile::Game ? m_Game : m_Toml;
	std::string v;
	if (!doc.Get(r.section, r.key, v))
	{
		// Sin HD en launcher.toml, la descarga sigue a las texturas HD del juego.
		if (r.file == OptionFile::Launcher && r.key == "HD")
		{
			std::string hd;
			return m_Game.Get(Opt::kHDTextures.section, Opt::kHDTextures.key, hd) && hd == "1" ? 1 : 0;
		}
		return r.defaultIndex;
	}
	if (r.file == OptionFile::Launcher && v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
	long n = 0;
	const bool isNum = r.numeric && ParseInt(v, n);
	for (std::size_t i = 0; i < r.choices.size(); ++i)
	{
		long c = 0;
		if (isNum && ParseInt(r.choices[i].value, c) ? c == n : EqualNoCase(r.choices[i].value, v)) return static_cast<int>(i);
	}
	return r.defaultIndex;   // fuera de rango o desconocido
}

int OptionsEditor::Current(std::size_t tab, std::size_t row) const
{
	const OptionRow& r = m_Tabs[tab].rows[row];
	if (r.text) return 0;
	const int sel = m_Selected[tab][row];
	return sel >= 0 ? sel : Stored(r);
}

bool OptionsEditor::Editable(std::size_t tab, std::size_t row) const
{
	return !(m_GameLocked && m_Tabs[tab].rows[row].file == OptionFile::Game);
}

void OptionsEditor::Select(std::size_t tab, std::size_t row, int index)
{
	const OptionRow& r = m_Tabs[tab].rows[row];
	if (!Editable(tab, row) || r.text || index < 0 || index >= static_cast<int>(r.choices.size())) return;
	m_Selected[tab][row] = index == Stored(r) ? -1 : index;
}

void OptionsEditor::Cycle(std::size_t tab, std::size_t row, int direction)
{
	const int n = static_cast<int>(m_Tabs[tab].rows[row].choices.size());
	if (n == 0) return;
	Select(tab, row, ((Current(tab, row) + direction) % n + n) % n);
}

bool OptionsEditor::Dirty() const
{
	for (const auto& t : m_Selected)
		for (int s : t)
			if (s >= 0) return true;
	return false;
}

bool OptionsEditor::Save(bool* downloadsChanged)
{
	if (downloadsChanged) *downloadsChanged = false;
	bool game = false, toml = false;
	for (std::size_t t = 0; t < m_Tabs.size(); ++t)
		for (std::size_t r = 0; r < m_Tabs[t].rows.size(); ++r)
		{
			const int sel = m_Selected[t][r];
			if (sel < 0) continue;
			const OptionRow& row = m_Tabs[t].rows[r];
			const std::string& value = row.text ? m_Text[t][r] : row.choices[static_cast<std::size_t>(sel)].value;
			if (row.file == OptionFile::Game)
			{
				game |= m_Game.Set(row.section, row.key, value);
				// Prender las texturas HD también las agrega a la descarga (y sin
				// HD en launcher.toml, la descarga sigue a esta opción).
				if (row.key == Opt::kHDTextures.key)
				{
					if (value == "1") toml |= m_Toml.Set("", "HD", "true");
					if (downloadsChanged) *downloadsChanged = true;
				}
			}
			else
			{
				toml |= m_Toml.Set("", row.key, value);
				if (downloadsChanged && row.key != "Scale") *downloadsChanged = true;
			}
		}
	bool ok = true;
	if (game) ok = m_Game.Save(m_GamePath) && ok;
	if (toml) ok = m_Toml.Save(m_TomlPath) && ok;
	if (ok) Revert();
	return ok;
}

} // namespace Launcher
