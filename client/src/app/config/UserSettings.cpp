// UserSettings.cpp — preferencias del jugador desde Config.ini. Ver UserSettings.h.

#include "stdafx.h"
#include "platform/Text.h"
#include "platform/IFileSystem.h"
#include "app/config/UserSettings.h"
#include "app/config/OptionTable.h"
#include "game/core/FrameLimiter.h"
#include "platform/IGLContext.h"
#include "render/Gfx.h"
#include <cerrno>
#include <climits>
#include <cctype>

extern "C" void DbgLogPublic(const char* msg);

CUserSettings gUserSettings;

namespace {

namespace Opt = UserOptionTable;

// Las tablas viven en OptionTable.h (las comparte el launcher).
static_assert(Opt::kResolutionCount == MAX_USER_RESOLUTION, "OptionTable: resoluciones");
static_assert(Opt::kLanguageCount == MAX_USER_LANGUAGE, "OptionTable: idiomas");
static_assert(Opt::kAntilagCount == MAX_ANTILAG, "OptionTable: antilag");
static_assert(Opt::kRenderBackendCount == static_cast<int>(RenderBackendPreference::Count), "OptionTable: backends");

// Ausente, truncado o inválido: no reemplazar el valor del registro/default.
bool TryReadInt(const char* section, const char* key, const char* iniPath, int& result)
{
    char value[64] = {};
    const DWORD length = Platform::GetFileSystem().ReadIniString(section, key, "", value, sizeof(value), iniPath);
    if (!length || length >= sizeof(value) - 1) return false;
    char* end;
    errno = 0;
    const long parsed = strtol(value, &end, 10);
    if (end == value || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX) return false;
    while (*end && isspace((unsigned char)*end)) ++end;
    if (*end) return false;
    result = (int)parsed;
    return true;
}

int ReadInt(const char* section, const char* key, const char* iniPath)
{
    int value;
    return TryReadInt(section, key, iniPath, value) ? value : -1;
}

int ReadRange(const char* section, const char* key, const char* iniPath,
              int low, int high, int fallback)
{
    int value;
    return TryReadInt(section, key, iniPath, value) && value >= low && value <= high
        ? value : fallback;
}

// Opción de la tabla: fuera de rango o ausente vale su default.
int ReadOption(const Opt::IntOption& o, const char* iniPath)
{
    return ReadRange(o.section, o.key, iniPath, o.min, o.max, o.def);
}

int ReadFlag(const char* section, const char* key, const char* iniPath)
{
    int value = ReadInt(section, key, iniPath);
    return value < 0 ? -1 : (value != 0);
}

const char* AntilagKey(int option) { return Opt::kAntilagKeys[option]; }

} // namespace

bool CUserSettings::GetResolutionSize(int index, DWORD* width, DWORD* height)
{
    if (index < 0 || index >= MAX_USER_RESOLUTION) return false;
    *width  = Opt::kResolutions[index].width;
    *height = Opt::kResolutions[index].height;
    return true;
}

void CUserSettings::Load(const char* iniPath)
{
    // Una segunda carga no debe conservar la fuente ni preferencias ausentes.
    *this = CUserSettings{};
    Platform::TextCopySecure(m_IniPath, iniPath);
    for (int i = 0; i < MAX_ANTILAG; ++i)
        m_Antilag[i] = ReadFlag("Antilag", AntilagKey(i), iniPath) > 0;
    m_WindowMode  = ReadFlag("Window", "WindowMode", iniPath);
    m_Borderless  = ReadFlag("Window", "Borderless", iniPath);
    m_Resolution  = ReadInt ("Window", "Resolution", iniPath);
    // DESVIACION: 0 desactiva el límite; entradas inválidas conservan 60 fps.
    m_FpsLimit = ReadOption(Opt::kFpsLimit, iniPath);
    m_RenderDecoupled = ReadOption(Opt::kRenderDecoupled, iniPath) != 0;
    m_VSync = ReadOption(Opt::kVSync, iniPath) != 0;
    m_GraphicsValidation = ReadOption(Opt::kGraphicsValidation, iniPath) != 0;
    char backend[16] = {};
    Platform::GetFileSystem().ReadIniString("Window", "RenderBackend", "opengl", backend, sizeof(backend), iniPath);
    for (int i = 0; i < static_cast<int>(RenderBackendPreference::Count); ++i)
        if (!Platform::TextCompareIgnoreCase(backend, RenderBackendKey(static_cast<RenderBackendPreference>(i))))
            m_RenderBackend = static_cast<RenderBackendPreference>(i);
    // Config.ini se comparte con Windows: un d3d12/d3d11 guardado allá se lee
    // acá como WebGPU automático, sin reescribir el archivo.
    if (!IsRenderBackendAvailable(m_RenderBackend)) m_RenderBackend = RenderBackendPreference::Auto;
    m_StartupRenderBackend = m_RenderBackend;
    m_TextureQuality = m_StartupTextureQuality = ReadOption(Opt::kTextureQuality, iniPath);
    m_Sharpness = ReadOption(Opt::kSharpness, iniPath);
    m_Antialiasing = m_StartupAntialiasing = ReadOption(Opt::kAntialiasing, iniPath);
    m_HDTextures = m_StartupHDTextures = ReadOption(Opt::kHDTextures, iniPath) != 0;
    m_Shadows = ReadOption(Opt::kShadows, iniPath);
    m_DepthFog = ReadOption(Opt::kDepthFog, iniPath) != 0;
    m_DynamicLights = ReadOption(Opt::kDynamicLights, iniPath);
    m_Bloom = ReadOption(Opt::kBloom, iniPath);
    m_Sky = ReadOption(Opt::kSky, iniPath) != 0;
    m_Underwater = ReadOption(Opt::kUnderwater, iniPath);
    m_WorldWater = ReadOption(Opt::kWorldWater, iniPath) != 0;
    m_DrawDistance = ReadOption(Opt::kDrawDistance, iniPath);
    if (m_Resolution < -1 || m_Resolution >= MAX_USER_RESOLUTION) {
        char line[96];
        Platform::TextFormat(line, "Config.ini: Resolution=%d IGNORADO (fuera de 0..%d)",
                  m_Resolution, MAX_USER_RESOLUTION - 1);
        DbgLogPublic(line);
        m_Resolution = -1;
    }

    m_EnableSound = ReadFlag("Sound", "EnableSound", iniPath);
    m_EnableMusic = ReadFlag("Sound", "EnableMusic", iniPath);
    // -1: no está; manda el registro.
    m_SoundLevel  = ReadRange(Opt::kSoundLevel.section, Opt::kSoundLevel.key, iniPath, Opt::kSoundLevel.min, Opt::kSoundLevel.max, -1);
    m_MusicLevel  = ReadRange(Opt::kMusicLevel.section, Opt::kMusicLevel.key, iniPath, Opt::kMusicLevel.min, Opt::kMusicLevel.max, -1);

    Platform::GetFileSystem().ReadIniString("User", "Username", "", m_Username, sizeof(m_Username), iniPath);
    m_MiniMapAlpha = ReadOption(Opt::kMiniMapAlpha, iniPath);
    m_MiniMapZoom  = ReadOption(Opt::kMiniMapZoom, iniPath);

    char language[8] = {};
    Platform::GetFileSystem().ReadIniString("Language", "LangSelection", "", language, sizeof(language), iniPath);
    for (int i = 0; language[0] && i < MAX_USER_LANGUAGE; ++i)
        if (Platform::TextCompareIgnoreCase(language, GetLanguageSuffix(i)) == 0) m_Language = i;
    if (language[0] && m_Language == USER_LANG_DEFAULT) {
        char line[96];
        Platform::TextFormat(line, "Config.ini: LangSelection=%s IGNORADO (Eng, Spn o Por)", language);
        DbgLogPublic(line);
    }

    // [Font]: si la sección existe, cada clave que falte toma el default del DLL.
    char section[8] = {};
    if (Platform::GetFileSystem().ReadIniSection("Font", section, sizeof(section), iniPath) > 0) {
        UserFontSettings& f = m_Font;
        f.present = true;
        Platform::GetFileSystem().ReadIniString("Font", "FontName", "Verdana", f.faceName, sizeof(f.faceName), iniPath);
        int height;
        if (TryReadInt("Font", "FontHeight", iniPath, height))
            f.height = height < -25 ? -25 : (height > 25 ? 25 : height);
        f.bold = ReadFlag("Font", "FontBold", iniPath) > 0;
        f.italic = ReadFlag("Font", "FontItalic", iniPath) > 0;
        f.charset   = ReadRange("Font", "FontCharset", iniPath, 0, 255, DEFAULT_CHARSET);
        f.width     = ReadRange("Font", "FontWidth", iniPath, -25, 25, 0);
        f.underline = ReadFlag("Font", "FontUnderline", iniPath) > 0;
        f.quality   = ReadRange("Font", "FontQuality", iniPath, 0, CLEARTYPE_NATURAL_QUALITY, NONANTIALIASED_QUALITY);
        f.strikeOut = ReadFlag("Font", "FontStrikeOut", iniPath) > 0;
    }

    // Independientes de que exista la fuente propia: valen con cualquiera.
    m_TextShadow = ReadFlag("Font", "TextShadow", iniPath) > 0;
    m_TextOutline = ReadFlag("Font", "TextOutline", iniPath) > 0;
    m_ColorEmoji = ReadFlag("Font", "ColorEmoji", iniPath) != 0;

    char line[160];
    Platform::TextFormat(line, "Config.ini: WindowMode=%d Borderless=%d Resolution=%d "
                    "EnableSound=%d EnableMusic=%d (-1 = no está)",
              m_WindowMode, m_Borderless, m_Resolution, m_EnableSound, m_EnableMusic);
    DbgLogPublic(line);
    if (m_Font.present) {
        Platform::TextFormat(line, "Config.ini: Font='%s' %d (bold=%d quality=%d)",
                  m_Font.faceName, m_Font.height, m_Font.bold, m_Font.quality);
    } else {
        Platform::TextFormat(line, "Config.ini: sin [Font], se usa la fuente del binario (Arial)");
    }
    DbgLogPublic(line);
}

const char* CUserSettings::GetLanguageSuffix(int language)
{
    return language >= 0 && language < MAX_USER_LANGUAGE ? Opt::kLanguageSuffixes[language] : nullptr;
}

void CUserSettings::SaveInt(const char* section, const char* key, int value) const
{
    if (!m_IniPath[0]) return;
    char text[16];
    Platform::TextFormat(text, "%d", value);
    Platform::GetFileSystem().WriteIniString(section, key, text, m_IniPath);
}

void CUserSettings::SetLanguage(int language)
{
    if (language < USER_LANG_DEFAULT || language >= MAX_USER_LANGUAGE) return;
    m_Language = language;
    // Sin selección se borra la clave: vuelven los archivos del 0.97k.
    if (m_IniPath[0])
        Platform::GetFileSystem().WriteIniString("Language", "LangSelection", GetLanguageSuffix(language), m_IniPath);
}

void CUserSettings::SetSoundLevel(int level)
{
    m_SoundLevel = level;
    SaveInt("Sound", "SoundLevel", level);
}

void CUserSettings::SetMusicLevel(int level)
{
    m_MusicLevel = level;
    SaveInt("Sound", "MusicLevel", level);
}

void CUserSettings::SetAntilag(eAntilag option, bool enabled)
{
    if (option < 0 || option >= MAX_ANTILAG) return;
    m_Antilag[option] = enabled;
    SaveInt("Antilag", AntilagKey(option), enabled ? 1 : 0);
}

const char* CUserSettings::RenderBackendKey(RenderBackendPreference preference)
{
    const int i = static_cast<int>(preference);
    return Opt::kRenderBackendKeys[i >= 0 && i < Opt::kRenderBackendCount ? i : 0];
}

bool CUserSettings::IsRenderBackendAvailable(RenderBackendPreference preference)
{
    // Con MinGW, Dawn se compila sólo con Vulkan (cmake/triplets/x64-mingw-mu):
    // igual que en Linux, D3D12/D3D11 no se ofrecen y un valor guardado cae en
    // Auto, que elige Vulkan.
#if defined(_WIN32) && !defined(__MINGW32__)
    return preference < RenderBackendPreference::Count;
#else
    return preference < RenderBackendPreference::Count &&
           preference != RenderBackendPreference::D3D12 && preference != RenderBackendPreference::D3D11;
#endif
}

const char* CUserSettings::RenderBackendName(RenderBackendPreference preference)
{
    const int i = static_cast<int>(preference);
    return Opt::kRenderBackendNames[i >= 0 && i < Opt::kRenderBackendCount ? i : 0];
}

void CUserSettings::SetTextureQuality(int level)
{
    if (!Opt::InRange(Opt::kTextureQuality, level)) return;
    m_TextureQuality = level;
    // Los mipmaps se generan al subir las texturas: valen los del arranque.
    Gfx::SetTextureQuality(m_StartupTextureQuality >= 1, level >= 2);
    SaveInt(Opt::kTextureQuality.section, Opt::kTextureQuality.key, level);
}

void CUserSettings::SetSharpness(int level)
{
    if (!Opt::InRange(Opt::kSharpness, level)) return;
    m_Sharpness = level;
    SaveInt(Opt::kSharpness.section, Opt::kSharpness.key, level);
}

void CUserSettings::SetHDTextures(bool enabled)
{
    m_HDTextures = enabled;
    SaveInt(Opt::kHDTextures.section, Opt::kHDTextures.key, enabled ? 1 : 0);
}

void CUserSettings::SetShadows(int level)
{
    if (!Opt::InRange(Opt::kShadows, level)) return;
    m_Shadows = level;
    SaveInt(Opt::kShadows.section, Opt::kShadows.key, level);
    Gfx::SetShadowMaps(level > 0, ShadowStrength());
}

void CUserSettings::SetDepthFog(bool enabled)
{
    m_DepthFog = enabled;
    SaveInt(Opt::kDepthFog.section, Opt::kDepthFog.key, enabled ? 1 : 0);
    Gfx::SetDepthFog(enabled);
}

void CUserSettings::SetDynamicLights(int level)
{
    if (!Opt::InRange(Opt::kDynamicLights, level)) return;
    m_DynamicLights = level;
    SaveInt(Opt::kDynamicLights.section, Opt::kDynamicLights.key, level);
}

void CUserSettings::SetBloom(int level)
{
    if (!Opt::InRange(Opt::kBloom, level)) return;
    m_Bloom = level;
    SaveInt(Opt::kBloom.section, Opt::kBloom.key, level);
}

void CUserSettings::SetSky(bool enabled)
{
    m_Sky = enabled;
    SaveInt(Opt::kSky.section, Opt::kSky.key, enabled ? 1 : 0);
}

void CUserSettings::SetUnderwater(int level)
{
    if (!Opt::InRange(Opt::kUnderwater, level)) return;
    m_Underwater = level;
    SaveInt(Opt::kUnderwater.section, Opt::kUnderwater.key, level);
}

void CUserSettings::SetWorldWater(bool enabled)
{
    m_WorldWater = enabled;
    Gfx::SetWater(enabled);
    SaveInt(Opt::kWorldWater.section, Opt::kWorldWater.key, enabled ? 1 : 0);
}

void CUserSettings::SetDrawDistance(int level)
{
    if (!Opt::InRange(Opt::kDrawDistance, level)) return;
    m_DrawDistance = level;
    SaveInt(Opt::kDrawDistance.section, Opt::kDrawDistance.key, level);
}

void CUserSettings::SetAntialiasingLevel(int level)
{
    if (!Opt::InRange(Opt::kAntialiasing, level)) return;
    m_Antialiasing = level;
    SaveInt(Opt::kAntialiasing.section, Opt::kAntialiasing.key, level);
}

bool CUserSettings::RestartPending() const
{
    return m_RenderBackend != m_StartupRenderBackend ||
        (m_TextureQuality >= 1) != (m_StartupTextureQuality >= 1) ||
        (m_Antialiasing >= 2) != (m_StartupAntialiasing >= 2) ||
        m_HDTextures != m_StartupHDTextures;
}

void CUserSettings::SetRenderBackend(RenderBackendPreference preference)
{
    if (preference >= RenderBackendPreference::Count) return;
    m_RenderBackend = preference;
    Platform::GetFileSystem().WriteIniString("Window", "RenderBackend", RenderBackendKey(preference), m_IniPath);
}

// DESVIACION: aplicar preferencias de dibujo en caliente sin reiniciar el reloj SIM.
// Si el driver rechaza VSync se conserva la preferencia anterior y no se guarda.
bool CUserSettings::SetPresentation(bool decoupled, bool vsync, int fpsLimit)
{
    if (fpsLimit<0) return false;
    if (vsync!=m_VSync && !gWindow.SetSwapInterval(vsync)) return false;
    m_RenderDecoupled=decoupled; m_VSync=vsync; m_FpsLimit=fpsLimit;
    gFrameLimiter.SetRenderDecoupled(decoupled);
    gFrameLimiter.SetFpsLimit(fpsLimit);
    SaveInt("Window","RenderDecoupled",decoupled ? 1 : 0);
    SaveInt("Window","VSync",vsync ? 1 : 0);
    SaveInt("Window","FpsLimit",fpsLimit);
    return true;
}

void CUserSettings::SetWindow(bool windowMode, bool borderless, int resolution)
{
    m_WindowMode = windowMode ? 1 : 0;
    m_Borderless = borderless ? 1 : 0;
    m_Resolution = resolution;
    SaveInt("Window", "WindowMode", m_WindowMode);
    SaveInt("Window", "Borderless", m_Borderless);
    if (resolution >= 0) SaveInt("Window", "Resolution", resolution);
}

int CUserSettings::FindResolution(DWORD width, DWORD height)
{
    for (int i = 0; i < MAX_USER_RESOLUTION; ++i)
        if (Opt::kResolutions[i].width == width && Opt::kResolutions[i].height == height) return i;
    return -1;
}

void CUserSettings::SetFont(const UserFontSettings& font)
{
    m_Font = font;
    m_Font.present = true;
    if (!m_IniPath[0]) return;
    Platform::GetFileSystem().WriteIniString("Font", "FontName", m_Font.faceName, m_IniPath);
    SaveInt("Font", "FontHeight", m_Font.height);
    SaveInt("Font", "FontBold", m_Font.bold);
    SaveInt("Font", "FontItalic", m_Font.italic);
    SaveInt("Font", "FontCharset", m_Font.charset);
    SaveInt("Font", "FontWidth", m_Font.width);
    SaveInt("Font", "FontUnderline", m_Font.underline);
    SaveInt("Font", "FontQuality", m_Font.quality);
    SaveInt("Font", "FontStrikeOut", m_Font.strikeOut);
}

void CUserSettings::SetTextEffects(bool shadow, bool outline, bool colorEmoji)
{
    m_TextShadow = shadow;
    m_TextOutline = outline;
    m_ColorEmoji = colorEmoji;
    SaveInt("Font", "TextShadow", shadow);
    SaveInt("Font", "TextOutline", outline);
    SaveInt("Font", "ColorEmoji", colorEmoji);
}

void CUserSettings::ResetFont()
{
    m_Font = UserFontSettings{};
    m_TextShadow = m_TextOutline = false;
    m_ColorEmoji = true;
    if (m_IniPath[0]) Platform::GetFileSystem().WriteIniString("Font", NULL, NULL, m_IniPath);
}

void CUserSettings::SetMiniMap(int alpha, int zoom)
{
    m_MiniMapAlpha = alpha;
    m_MiniMapZoom = zoom;
    SaveInt("MiniMap", "Alpha", alpha);
    SaveInt("MiniMap", "ZoomLevel", zoom);
}
