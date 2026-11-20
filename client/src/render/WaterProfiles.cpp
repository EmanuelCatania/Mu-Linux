#include "stdafx.h"
#include "render/WaterProfiles.h"
#include "game/core/MapNumber.h"
#include <algorithm>

namespace {
// La textura 5 del terreno es el agua en todos los mapas: FaceTexture le
// anima las UV y el render del terreno la marca como capa de agua.
constexpr unsigned char WaterTile = 5;

struct Profile {
    int map;
    bool enabled;
    Gfx::WaterLook look;
};
// Lo que no está en la tabla usa este.
constexpr Gfx::WaterLook DefaultLook{
    {0.10f, 0.20f, 0.24f}, 0.35f,
    {0.55f, 0.66f, 0.74f}, 0.7f,
    {0.35f, 0.55f, 0.76f},
};
const Profile Profiles[] = {
    // Bajo el agua: tiene su propia niebla y su capa de agua aparte.
    {MAP_ATLANS, false, DefaultLook},
    // Sin terreno: el mapa son nubes.
    {MAP_ICARUS, false, DefaultLook},
    // Nieve: agua más fría y cielo más blanco.
    {MAP_DEVIAS, true, {{0.12f, 0.20f, 0.26f}, 0.35f, {0.72f, 0.78f, 0.84f}, 0.6f, {0.35f, 0.55f, 0.76f}}},
};
}

namespace WaterProfiles {
bool Current(int map, unsigned char mask[256 * 256 * 4], Gfx::WaterLook& look) {
    look = DefaultLook;
    for (const Profile& p : Profiles) {
        if (p.map != map) continue;
        if (!p.enabled) return false;
        look = p.look;
        break;
    }
    // Igual que Terrain_RenderFace: el tile (x, y) usa las capas de su vértice
    // (x, y). Si sus cuatro vértices tienen alfa 1 se pinta entero con la capa
    // 2; si no, base = capa 1 y encima la capa 2 con el alfa de cada vértice.
    auto alpha = [](int x, int y) {
        return std::clamp(TerrainMappingAlpha[(y & 255) * 256 + (x & 255)], 0.0f, 1.0f);
    };
    bool any = false;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int i = y * 256 + x;
            const bool full = alpha(x, y) >= 1.0f && alpha(x + 1, y) >= 1.0f &&
                              alpha(x, y + 1) >= 1.0f && alpha(x + 1, y + 1) >= 1.0f;
            const bool over = TerrainMappingLayer2[i] == WaterTile;
            const bool base = full ? over : TerrainMappingLayer1[i] == WaterTile;
            unsigned char* o = mask + size_t(i) * 4;
            o[0] = base ? 255 : 0;
            o[1] = over ? 255 : 0;
            o[2] = (unsigned char)(alpha(x, y) * 255.0f + 0.5f);
            o[3] = 255;
            any = any || base || over;
        }
    return any;
}
}
