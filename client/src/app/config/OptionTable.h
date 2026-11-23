#pragma once

// Tabla de las opciones de Config.ini que comparten el cliente
// (app/config/UserSettings) y el launcher (src/launcher, pestañas de opciones).
//
// Sección, clave, rango y valor por defecto viven sólo acá: los dos lados leen
// y validan con la misma tabla, así no pueden divergir.  Sin dependencias (el
// launcher no incluye el stdafx del cliente).

namespace UserOptionTable {

// Opción entera.  Un valor guardado fuera de [min, max] se ignora y vale def.
struct IntOption
{
	const char* section;
	const char* key;
	int min;
	int max;
	int def;
};

// [Window]
// WindowMode, Borderless y Resolution: si faltan manda el registro del 0.97k;
// def es lo que muestra el launcher (la plantilla Config.ini los trae).
constexpr IntOption kWindowMode{ "Window", "WindowMode", 0, 1, 1 };
constexpr IntOption kBorderless{ "Window", "Borderless", 0, 1, 0 };
constexpr IntOption kRenderDecoupled{ "Window", "RenderDecoupled", 0, 1, 1 };
constexpr IntOption kVSync{ "Window", "VSync", 0, 1, 0 };
constexpr IntOption kGraphicsValidation{ "Window", "GraphicsValidation", 0, 1, 0 };
constexpr IntOption kTextureQuality{ "Window", "TextureQuality", 0, 2, 0 };
constexpr IntOption kSharpness{ "Window", "Sharpness", 0, 3, 0 };
constexpr IntOption kAntialiasing{ "Window", "Antialiasing", 0, 3, 0 };
constexpr IntOption kHDTextures{ "Window", "HDTextures", 0, 1, 0 };
constexpr IntOption kShadows{ "Window", "Shadows", 0, 2, 0 };
constexpr IntOption kDepthFog{ "Window", "DepthFog", 0, 1, 0 };
constexpr IntOption kDynamicLights{ "Window", "DynamicLights", 0, 2, 1 };
constexpr IntOption kBloom{ "Window", "Bloom", 0, 2, 1 };
constexpr IntOption kSky{ "Window", "Sky", 0, 1, 1 };
constexpr IntOption kUnderwater{ "Window", "Underwater", 0, 2, 2 };
constexpr IntOption kWorldWater{ "Window", "WorldWater", 0, 1, 1 };
constexpr IntOption kDrawDistance{ "Window", "DrawDistance", 0, 3, 0 };
// 0: sin límite.
constexpr IntOption kFpsLimit{ "Window", "FpsLimit", 0, 2147483647, 60 };

// [Sound]: si faltan EnableSound/EnableMusic o los niveles, manda el registro.
constexpr IntOption kEnableSound{ "Sound", "EnableSound", 0, 1, 1 };
constexpr IntOption kEnableMusic{ "Sound", "EnableMusic", 0, 1, 1 };
constexpr IntOption kSoundLevel{ "Sound", "SoundLevel", 0, 9, 8 };
constexpr IntOption kMusicLevel{ "Sound", "MusicLevel", 0, 9, 6 };

// [MiniMap] (defaults del DLL)
constexpr IntOption kMiniMapAlpha{ "MiniMap", "Alpha", 3, 10, 10 };
constexpr IntOption kMiniMapZoom{ "MiniMap", "ZoomLevel", 0, 6, 0 };

// [Font] efectos (valen con cualquier fuente).
constexpr IntOption kTextShadow{ "Font", "TextShadow", 0, 1, 0 };
constexpr IntOption kTextOutline{ "Font", "TextOutline", 0, 1, 0 };
constexpr IntOption kColorEmoji{ "Font", "ColorEmoji", 0, 1, 1 };

// [Window] Resolution: índice en esta tabla.  Los cuatro primeros coinciden con
// los del registro del 0.97k; el 4 no (ahí era 1600x1200).  Misma tabla que
// CWindow::iResolutionValues del DLL, más 3440x1440 al final.
struct Resolution
{
	unsigned width;
	unsigned height;
};
constexpr Resolution kResolutions[] = {
	{ 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 1024 },
	{ 1280, 720 }, { 1366, 768 }, { 1600, 900 }, { 1920, 1080 },
	{ 3440, 1440 },
};
constexpr int kResolutionCount = static_cast<int>(sizeof(kResolutions) / sizeof(kResolutions[0]));
constexpr IntOption kResolution{ "Window", "Resolution", 0, kResolutionCount - 1, 3 };

// [Window] RenderBackend: nombre guardado, en el orden de RenderBackendPreference.
constexpr const char* kRenderBackendKeys[] = { "opengl", "auto", "d3d12", "d3d11", "vulkan" };
constexpr const char* kRenderBackendNames[] = { "OpenGL", "WebGPU", "DirectX 12", "DirectX 11", "Vulkan" };
constexpr int kRenderBackendCount = 5;

// [Language] LangSelection, en el orden de eUserLanguage.
constexpr const char* kLanguageSuffixes[] = { "Eng", "Spn", "Por" };
constexpr int kLanguageCount = 3;

// [Antilag], en el orden de eAntilag.  0|1, por defecto 0.
constexpr const char* kAntilagKeys[] = {
	"DeleteShadows", "DeleteObjects", "DeleteFloor", "DeleteSkills",
	"DeleteStaticEffects", "DeleteDynamicEffects", "DeleteWings",
	"DeleteHealthBar", "DeleteInterface", "DeleteWeather", "DeleteGlow",
};
constexpr int kAntilagCount = static_cast<int>(sizeof(kAntilagKeys) / sizeof(kAntilagKeys[0]));

constexpr bool InRange(const IntOption& o, int v) { return v >= o.min && v <= o.max; }

} // namespace UserOptionTable
