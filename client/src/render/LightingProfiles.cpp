#include "stdafx.h"
#include "render/LightingProfiles.h"
#include "assets/data/TomlLines.h"
#include "game/core/MapInfoTable.h"
#include "game/core/MapNumber.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void DbgLogPublic(const char* msg);

namespace LightingProfiles {
namespace {

using Fields = std::vector<std::pair<std::string, std::string>>;

// Interiores: sin cielo y con sombras suaves, porque no todas las zonas van a
// tener una llama cerca (sin ninguna sombra lo que queda lejos se ve plano).
Look Interior()
{
    Look l;
    l.sky = 0.0f;
    l.sunShadow = 0.45f;
    l.shadowSoftness = 2.5f;
    l.base[0] = 0.92f; l.base[1] = 0.62f;
    l.gain[0] = 1.2f; l.gain[1] = 2.0f;
    l.radius[0] = 1.4f; l.radius[1] = 1.8f;
    return l;
}

Look Sky(const float horizon[3], const float zenith[3], const float sun[3], float clouds)
{
    Look l;
    std::memcpy(l.horizon, horizon, sizeof(l.horizon));
    std::memcpy(l.zenith, zenith, sizeof(l.zenith));
    std::memcpy(l.sunColor, sun, sizeof(l.sunColor));
    l.clouds = clouds;
    return l;
}

Look Default(int map)
{
    switch (map) {
    case MAP_LORENCIA: {   // atardecer cálido
        const float h[3] = {0.86f, 0.62f, 0.44f}, z[3] = {0.24f, 0.34f, 0.55f}, s[3] = {1.0f, 0.74f, 0.45f};
        return Sky(h, z, s, 0.35f);
    }
    case MAP_DUNGEON:
    case MAP_LOST_TOWER:
        return Interior();
    case MAP_DEVIAS: {     // frío, nublado
        const float h[3] = {0.76f, 0.80f, 0.86f}, z[3] = {0.42f, 0.52f, 0.66f}, s[3] = {0.95f, 0.95f, 1.0f};
        Look l = Sky(h, z, s, 0.65f);
        l.bloom[0] = 0.35f; l.bloom[1] = 0.5f;   // la nieve es clara: casi todo pasaría el umbral
        return l;
    }
    case MAP_NORIA: {      // verde claro
        const float h[3] = {0.72f, 0.84f, 0.66f}, z[3] = {0.30f, 0.55f, 0.64f}, s[3] = {1.0f, 0.95f, 0.78f};
        return Sky(h, z, s, 0.3f);
    }
    case MAP_EXILE:
    case MAP_ARENA: {
        const float h[3] = {0.55f, 0.50f, 0.50f}, z[3] = {0.16f, 0.18f, 0.28f}, s[3] = {1.0f, 0.80f, 0.60f};
        return Sky(h, z, s, 0.5f);
    }
    case MAP_ATLANS: {     // bajo el agua: sin cielo, cáusticas y tinte
        Look l;
        l.sky = 0.0f;
        l.sunShadow = 0.6f;
        l.shadowSoftness = 2.0f;
        l.underwater = 1.0f;
        l.caustics = 0.55f;
        l.tint[0] = 0.70f; l.tint[1] = 0.93f; l.tint[2] = 1.06f;
        l.rays = 0.35f;
        l.bloom[0] = 0.5f; l.bloom[1] = 0.7f;   // el fondo claro con cáusticas pasa el umbral
        return l;
    }
    case MAP_TARKAN: {     // desierto
        const float h[3] = {0.88f, 0.73f, 0.52f}, z[3] = {0.42f, 0.50f, 0.64f}, s[3] = {1.0f, 0.86f, 0.62f};
        Look l = Sky(h, z, s, 0.15f);
        l.bloom[0] = 0.5f; l.bloom[1] = 0.7f;    // arena clara
        return l;
    }
    case MAP_DEVIL_SQUARE1: {   // exterior bajo lluvia
        const float h[3] = {0.30f, 0.32f, 0.36f}, z[3] = {0.11f, 0.12f, 0.15f}, s[3] = {0.6f, 0.62f, 0.7f};
        Look l = Sky(h, z, s, 0.95f);
        l.rain = 1.0f;
        l.sunShadow = 0.5f;
        l.shadowSoftness = 1.6f;
        return l;
    }
    case MAP_ICARUS: {     // cielo abierto
        const float h[3] = {0.74f, 0.84f, 0.96f}, z[3] = {0.22f, 0.42f, 0.76f}, s[3] = {1.0f, 0.95f, 0.85f};
        return Sky(h, z, s, 0.3f);
    }
    default:
        if (IsBloodCastleMap(map)) {   // noche; la estatua es una zona interior
            const float h[3] = {0.26f, 0.20f, 0.28f}, z[3] = {0.06f, 0.05f, 0.10f}, s[3] = {0.8f, 0.6f, 0.7f};
            return Sky(h, z, s, 0.6f);
        }
        return Look{};
    }
}

// Zonas de la tabla (para un mapa sin zonas en maps.toml). Blood Castle: el camino es exterior; del muro de la puerta (y = 76) hacia
// adentro es el castillo, donde está la estatua.
bool DefaultZoneAt(int map, int x, int y, Look& out)
{
    if (!IsBloodCastleMap(map)) return false;
    if (x < 0 || x > 40 || y < 76 || y > 110) return false;
    out = Interior();
    return true;
}

bool ParseVec(const std::string& text, float* out, int count)
{
    std::vector<std::string> items;
    if (!TomlLines::SplitArray(text, items) || (int)items.size() != count) return false;
    for (int i = 0; i < count; ++i)
        if (!TomlLines::ParseFloat(items[i], out[i])) return false;
    return true;
}

void Apply(const Fields& fields, Look& l, int map)
{
    for (const auto& [key, value] : fields) {
        bool ok = false, flag = false;
        if (key == "sky") { ok = TomlLines::ParseBool(value, flag); l.sky = flag ? 1.0f : 0.0f; }
        else if (key == "underwater") { ok = TomlLines::ParseBool(value, flag); l.underwater = flag ? 1.0f : 0.0f; }
        else if (key == "horizon") ok = ParseVec(value, l.horizon, 3);
        else if (key == "zenith") ok = ParseVec(value, l.zenith, 3);
        else if (key == "sun_color") ok = ParseVec(value, l.sunColor, 3);
        else if (key == "tint") ok = ParseVec(value, l.tint, 3);
        else if (key == "clouds") ok = TomlLines::ParseFloat(value, l.clouds);
        else if (key == "rain") ok = TomlLines::ParseFloat(value, l.rain);
        else if (key == "sun_shadow") ok = TomlLines::ParseFloat(value, l.sunShadow);
        else if (key == "shadow_softness") ok = TomlLines::ParseFloat(value, l.shadowSoftness);
        else if (key == "caustics") ok = TomlLines::ParseFloat(value, l.caustics);
        else if (key == "rays") ok = TomlLines::ParseFloat(value, l.rays);
        else if (key == "base") ok = ParseVec(value, l.base, 2);
        else if (key == "gain") ok = ParseVec(value, l.gain, 2);
        else if (key == "radius") ok = ParseVec(value, l.radius, 2);
        else if (key == "bloom") ok = ParseVec(value, l.bloom, 2);
        if (!ok)
            DbgLogPublic(("LightingProfiles: [map." + std::to_string(map) + ".lighting] " + key +
                          " = " + value + " inválido").c_str());
    }
}

// Perfil base del mapa (tabla + maps.toml), armado una vez por mapa.
const Look& MapLook(int map)
{
    static std::unordered_map<int, Look> cache;
    auto found = cache.find(map);
    if (found != cache.end()) return found->second;
    Look l = Default(map);
    Apply(MapInfo_Get(map).lighting, l, map);
    return cache.emplace(map, l).first->second;
}

// Perfil en la posición: la primera zona de maps.toml que la cubra; si no
// hay ninguna configurada, las zonas de la tabla.
Look LookAt(int map, int tile, int x, int y)
{
    const MapInfo& info = MapInfo_Get(map);
    for (const MapLightingZone& zone : info.lightingZones) {
        const bool inTile = tile >= 0 && tile < 256 && zone.tiles.test(tile);
        const bool inRect = zone.rect[0] >= 0 && x >= zone.rect[0] && x <= zone.rect[2] &&
                            y >= zone.rect[1] && y <= zone.rect[3];
        if (!inTile && !inRect) continue;
        Look l = MapLook(map);
        Apply(zone.fields, l, map);
        return l;
    }
    Look l;
    if (info.lightingZones.empty() && DefaultZoneAt(map, x, y, l)) return l;
    return MapLook(map);
}

void Blend(Look& from, const Look& to, float k)
{
    float* a = reinterpret_cast<float*>(&from);
    const float* b = reinterpret_cast<const float*>(&to);
    for (size_t i = 0; i < sizeof(Look) / sizeof(float); ++i) a[i] += (b[i] - a[i]) * k;
}
static_assert(sizeof(Look) % sizeof(float) == 0, "Look sólo lleva floats");

}

const Look& Current(int map, int heroTile, int gridX, int gridY)
{
    static Look current;
    static int currentMap = -1;
    static auto last = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(now - last).count();
    last = now;
    const Look target = LookAt(map, heroTile, gridX, gridY);
    if (map != currentMap) {
        // Al entrar a un mapa no hay fundido desde el anterior.
        current = target;
        currentMap = map;
        return current;
    }
    // Medio segundo para llegar casi entero (exponencial).
    Blend(current, target, std::clamp(dt * 6.0f, 0.0f, 1.0f));
    return current;
}

}
