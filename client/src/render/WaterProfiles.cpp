#include "stdafx.h"
#include "render/WaterProfiles.h"
#include "game/core/MapNumber.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
// La textura 5 del terreno (TileWater01) es la que FaceTexture anima como
// agua. En casi todos los mapas es agua, pero no en todos: en Dungeon es roca
// oscura, en Lost Tower lava y en Tarkan arena (revisado 2026-11-20 con las
// texturas de cada mapa). Esos van apagados en la tabla.
constexpr unsigned char WaterTile = 5;

struct Profile {
    int map;
    bool enabled;
    Gfx::WaterLook look;
    // Cauces hondos: el agua sube sobre un fondo tallado. Sin ellos toda el
    // agua del mapa va plana, sobre el agua que pinta el mapa.
    bool deep = true;
    // Agua pintada en laderas (los arroyos de Devias y de Noria): el plano
    // queda bajo el suelo y sólo asoma en líneas o pedazos. Con esto la
    // superficie sube hasta el suelo en cada vértice de los tiles de agua y los
    // cubre enteros.
    bool drape = false;
};
// Lo que no está en la tabla usa este (el agua verdosa de Lorencia).
constexpr Gfx::WaterLook DefaultLook{
    {0.10f, 0.20f, 0.24f},
    {0.55f, 0.66f, 0.74f}, 0.7f,
    {0.35f, 0.55f, 0.76f},
};
const Profile Profiles[] = {
    // La textura 5 no es agua.
    {MAP_DUNGEON, false, DefaultLook},
    {MAP_LOST_TOWER, false, DefaultLook},
    {MAP_TARKAN, false, DefaultLook},
    // Bajo el agua: tiene su propia niebla y su capa de agua aparte.
    {MAP_ATLANS, false, DefaultLook},
    // Sin terreno: el mapa son nubes.
    {MAP_ICARUS, false, DefaultLook},
    // Nieve: el agua original es gris clara y casi helada; cielo blanco.
    {MAP_DEVIAS, true, {{0.20f, 0.27f, 0.31f}, {0.78f, 0.82f, 0.86f}, 0.5f, {0.35f, 0.55f, 0.76f}}, true, true},
    // Arroyos poco hondos de agua azul brillante: sin cauce (las orillas en
    // escalón quedaban tapadas) y apoyados en el suelo, porque la mayoría baja
    // por una pendiente y un plano dejaba seco el 30 % del agua del mapa.
    {MAP_NORIA, true, {{0.05f, 0.13f, 0.27f}, {0.50f, 0.64f, 0.80f}, 0.7f, {0.35f, 0.55f, 0.76f}}, false, true},
};
}

// Cauce: profundidad máxima en el medio del agua y cuántos tiles tarda en
// llegar a ella desde la orilla.
constexpr float CarveDepth = 120.0f;
constexpr float CarveRamp = 4.0f;
float carve[256 * 256];
static unsigned char vertexLevel[256 * 256];
static unsigned char deepVertex[256 * 256];
unsigned char wallTexture[256 * 256];
bool carveActive = false;
int carveMap = -1;
float carveSum = 0.0f;

namespace WaterProfiles {
const float* Carve() { return carveActive ? carve : nullptr; }
const unsigned char* WallTexture() { return carveActive ? wallTexture : nullptr; }
float DeepSurfaceAt(float x, float y) {
    if (!carveActive) return -1.0e9f;
    const float gx = x / 100.0f, gy = y / 100.0f;
    const int ix = (int)gx, iy = (int)gy;
    if (ix < 0 || iy < 0 || ix > 254 || iy > 254) return -1.0e9f;
    const int i = iy * 256 + ix;
    if (!deepVertex[i] && !deepVertex[i + 1] && !deepVertex[i + 256] && !deepVertex[i + 257]) return -1.0e9f;
    // Igual que levelAt del shader: (G - 20) * 2 + 8, interpolado.
    auto at = [](int k) { return vertexLevel[k] ? (vertexLevel[k] - 20) * 2.0f + 8.0f : -60.0f; };
    const float fx = gx - ix, fy = gy - iy;
    const float a = at(i) + (at(i + 1) - at(i)) * fx;
    const float b = at(i + 256) + (at(i + 257) - at(i + 256)) * fx;
    return a + (b - a) * fy;
}
void DisableCarve() { carveActive = false; }

bool Current(int map, unsigned char mask[256 * 256 * 4], Gfx::WaterLook& look) {
    carveActive = false;
    look = DefaultLook;
    bool allowDeep = true;
    bool drape = false;
    for (const Profile& p : Profiles) {
        if (p.map != map) continue;
        if (!p.enabled) return false;
        look = p.look;
        allowDeep = p.deep;
        drape = p.drape;
        break;
    }
    // Sin cauces el agua se redibuja entera: el arroyo del mapa tiene la orilla
    // en escalones de tile y no hay fondo tallado que la redondee.
    look.redraw = allowDeep ? 0.0f : 1.0f;
    // Igual que Terrain_RenderFace: el tile (x, y) usa las capas de su vértice
    // (x, y). Si sus cuatro vértices tienen alfa 1 se pinta entero con la capa
    // 2; si no, base = capa 1 y encima la capa 2 con el alfa de cada vértice.
    auto alpha = [](int x, int y) {
        return std::clamp(TerrainMappingAlpha[(y & 255) * 256 + (x & 255)], 0.0f, 1.0f);
    };
    static unsigned char tile[256 * 256];
    bool any = false;
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int i = y * 256 + x;
            const bool full = alpha(x, y) >= 1.0f && alpha(x + 1, y) >= 1.0f &&
                              alpha(x, y + 1) >= 1.0f && alpha(x + 1, y + 1) >= 1.0f;
            const bool over = TerrainMappingLayer2[i] == WaterTile;
            const bool base = full ? over : TerrainMappingLayer1[i] == WaterTile;
            tile[i] = (base ? 1 : 0) | (over ? 2 : 0);
            any = any || tile[i] != 0;
        }
    // Nivel del agua (se rearma sólo al cambiar de mapa o de alturas).
    // Un río del mapa no es sólo sus tiles de agua: es toda la zona no
    // caminable (TerrainWall & 4) que los rodea, con barrancas hasta el borde
    // por donde se camina. Si ese borde (de lo caminable en 6 tiles a la
    // redonda) está más de DeepBank por encima del agua que pinta el mapa, es
    // un cauce y la superficie sube hasta la mitad de la barranca (FillRatio):
    // en el foso de Lorencia unas 70 bajo el camino, en un arroyo con orillas
    // bajas apenas lo justo para que el borde siga la orilla y no los tiles.
    constexpr float FillRatio = 0.5f;
    constexpr float DeepBank = 20.0f;
    static float tileLow[256 * 256];
    static int levelMap = -1;
    static float levelSum = 0.0f;
    auto isWater = [&](int tx, int ty) {
        return tx >= 0 && ty >= 0 && tx < 255 && ty < 255 && tile[ty * 256 + tx] != 0;
    };
    float heightSum = 0.0f;
    for (int i = 0; i < 256 * 256; i += 97) heightSum += BackTerrainHeight[i];
    if (map != levelMap || heightSum != levelSum) {
        levelMap = map;
        levelSum = heightSum;
        carveMap = -1;  // el cauce depende de esto
        static float tileTop[256 * 256];
        for (int ty = 0; ty < 255; ++ty)
            for (int tx = 0; tx < 255; ++tx) {
                const int c = ty * 256 + tx;
                tileLow[c] = (std::min)((std::min)(BackTerrainHeight[c], BackTerrainHeight[c + 1]),
                                        (std::min)(BackTerrainHeight[c + 256], BackTerrainHeight[c + 257]));
                // Lo mínimo: el borde de arriba de los tiles de agua de la
                // orilla (las paredes de ~140 bajo los puentes no cuentan).
                // Ya no se sube hasta el borde de arriba del agua pintada: debajo
                // de los puentes de madera ese borde llega casi al tablón y lo
                // inundaba. La altura la da el borde caminable (rim).
                tileTop[c] = tileLow[c];
            }
        // Tiles del río: agua, o no caminables a 3 tiles o menos de agua.
        static unsigned char river[256 * 256];
        for (int ty = 0; ty < 255; ++ty)
            for (int tx = 0; tx < 255; ++tx) {
                const int c = ty * 256 + tx;
                bool r = isWater(tx, ty);
                if (!r && (TerrainWall[c] & 4))
                    for (int dy = -3; dy <= 3 && !r; ++dy)
                        for (int dx = -3; dx <= 3; ++dx)
                            if (isWater(tx + dx, ty + dy)) { r = true; break; }
                river[c] = r ? 1 : 0;
            }
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const int i = y * 256 + x;
                bool inRiver = false;
                for (int ty = y - 1; ty <= y && !inRiver; ++ty)
                    for (int tx = x - 1; tx <= x; ++tx)
                        if (tx >= 0 && ty >= 0 && tx < 255 && ty < 255 && river[ty * 256 + tx]) { inRiver = true; break; }
                if (!inRiver) { vertexLevel[i] = 0; deepVertex[i] = 0; continue; }
                // Borde: no el punto caminable más bajo (un islote o el pie de
                // una muralla hundían el nivel y dejaban pozos) sino uno de los
                // más bajos (el 30 %) de los caminables de alrededor.
                // Altura del agua del mapa en este punto: la de los tiles de
                // agua más cercanos (no el más alto de alrededor: en un río que
                // baja, el de aguas arriba desbordaba las orillas de abajo).
                float top = -1.0e9f;
                for (int r = 1; r <= 6 && top < -1.0e8f; ++r) {
                    float sum = 0.0f;
                    int n = 0;
                    for (int ty = y - r; ty <= y + r - 1; ++ty)
                        for (int tx = x - r; tx <= x + r - 1; ++tx) {
                            if (tx < 0 || ty < 0 || tx > 254 || ty > 254) continue;
                            const int c = ty * 256 + tx;
                            if (tile[c]) { sum += tileTop[c]; ++n; }
                        }
                    if (n) top = sum / n;
                }
                float walk[144];
                int walkCount = 0;
                for (int ty = y - 6; ty <= y + 5; ++ty)
                    for (int tx = x - 6; tx <= x + 5; ++tx) {
                        if (tx < 0 || ty < 0 || tx > 254 || ty > 254) continue;
                        const int c = ty * 256 + tx;
                        if (!(TerrainWall[c] & 4) && !tile[c]) walk[walkCount++] = tileLow[c];
                    }
                if (top < -1.0e8f) top = BackTerrainHeight[i];
                float level = top;
                if (walkCount > 0) {
                    std::nth_element(walk, walk + walkCount * 3 / 10, walk + walkCount);
                    const float rim = walk[walkCount * 3 / 10];
                    // Sólo un cauce (orillas más altas que el agua) sube el agua;
                    // en lo plano (charcos a ras del suelo) queda a la altura del
                    // agua del mapa.
                    if (allowDeep && rim - top > DeepBank) level = top + (rim - top) * FillRatio;
                }
                vertexLevel[i] = (unsigned char)std::clamp((int)(level * 0.5f + 0.5f) + 20, 1, 255);
                deepVertex[i] = level > top + 1.0f ? 1 : 0;
            }
        // Pozos: una zona chica que quedó a ras del agua rodeada de cauce hondo
        // (un islote caminable dentro del foso) toma el nivel del cauce. Las
        // zonas planas grandes (arroyos, charcos) quedan como están.
        {
            std::vector<int> stack, area;
            static unsigned char seen[256 * 256];
            std::fill(std::begin(seen), std::end(seen), (unsigned char)0);
            for (int start = 0; start < 256 * 256; ++start) {
                if (!vertexLevel[start] || deepVertex[start] || seen[start]) continue;
                stack.assign(1, start);
                area.clear();
                seen[start] = 1;
                unsigned char around = 0;
                while (!stack.empty()) {
                    const int i = stack.back();
                    stack.pop_back();
                    area.push_back(i);
                    const int x = i & 255, y = i >> 8;
                    const int next[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
                    for (const auto& n : next) {
                        if (n[0] < 0 || n[1] < 0 || n[0] > 255 || n[1] > 255) continue;
                        const int j = n[1] * 256 + n[0];
                        if (!vertexLevel[j]) continue;
                        if (deepVertex[j]) { around = (std::max)(around, vertexLevel[j]); continue; }
                        if (!seen[j]) { seen[j] = 1; stack.push_back(j); }
                    }
                }
                if (around && area.size() <= 80)
                    for (int i : area) { vertexLevel[i] = around; deepVertex[i] = 1; }
            }
        }
        static unsigned char raised[256 * 256];
        // Al borde del río el nivel no puede caer a "sin agua" de un vértice al
        // siguiente: el plano quedaría inclinado y el agua parecería caer. Los
        // vértices de afuera que están a la altura del agua o más arriba toman
        // el nivel de al lado; el terreno, más alto, corta el agua en el borde.
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const int i = y * 256 + x;
                if (vertexLevel[i]) { raised[i] = vertexLevel[i]; continue; }
                unsigned char v = 0;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx) {
                        const int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx > 255 || ny > 255) continue;
                        v = (std::max)(v, vertexLevel[ny * 256 + nx]);
                    }
                raised[i] = (v && BackTerrainHeight[i] >= (v - 20) * 2.0f - 5.0f) ? v : 0;
            }
        std::copy(std::begin(raised), std::end(raised), std::begin(vertexLevel));
        // Laderas pintadas de agua: cada vértice de un tile de agua que quedó
        // por encima del plano lo sube hasta el suelo. Hasta 60: más alto es
        // una pared (bajo un puente) y ésa la cambia de textura WallTexture.
        if (drape && allowDeep)
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    const int i = y * 256 + x;
                    if (!vertexLevel[i]) continue;
                    bool wet = false;
                    for (int ty = y - 1; ty <= y && !wet; ++ty)
                        for (int tx = x - 1; tx <= x; ++tx)
                            if (tx >= 0 && ty >= 0 && tx < 255 && ty < 255 && tile[ty * 256 + tx]) { wet = true; break; }
                    if (!wet) continue;
                    const float now = (vertexLevel[i] - 20) * 2.0f;
                    const float ground = BackTerrainHeight[i];
                    if (ground <= now || ground - now > 60.0f) continue;
                    vertexLevel[i] = (unsigned char)std::clamp((int)(ground * 0.5f + 0.5f) + 20, 1, 255);
                }
        // Redibujada: no puede asomar suelo dentro de la orilla. La orilla (B,
        // interpolada) llega un vértice más allá de los tiles de agua, así que
        // también suben ésos, sin tope y redondeando para arriba: el shader
        // interpola con los triángulos del terreno y queda siempre encima.
        if (drape && !allowDeep)
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    bool nearWater = false;
                    for (int ty = y - 2; ty <= y + 1 && !nearWater; ++ty)
                        for (int tx = x - 2; tx <= x + 1; ++tx)
                            if (isWater(tx, ty)) { nearWater = true; break; }
                    if (!nearWater) continue;
                    const int i = y * 256 + x;
                    const int ground = std::clamp((int)std::ceil(BackTerrainHeight[i] * 0.5f) + 20, 1, 255);
                    vertexLevel[i] = (unsigned char)(std::max)((int)vertexLevel[i], ground);
                }
    }
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            const int i = y * 256 + x;
            unsigned char* o = mask + size_t(i) * 4;
            // R por tile: bits de agua (1 base, 2 capa 2) + 4 si es cauce hondo
            // (alguna esquina sube el agua), en pasos de 36.
            bool tileDeep = false;
            if (x < 255 && y < 255)
                tileDeep = deepVertex[i] || deepVertex[i + 1] || deepVertex[i + 256] || deepVertex[i + 257];
            o[0] = (unsigned char)((tile[i] + (tileDeep ? 4 : 0)) * 36);
            o[1] = vertexLevel[i];
            // B por vértice: el alfa con que la capa 2 se mezcla sobre la base,
            // igual que el terreno. Con él la orilla sale donde el mapa la pintó
            // fundida (pasto sobre agua) en lugar de en escalones de tile.
            if (allowDeep) {
                o[2] = (unsigned char)(alpha(x, y) * 255.0f + 0.5f);
            } else {
                // Redibujada: B = cuánta agua deja ver el mapa en los cuatro
                // tiles del vértice (la base y la capa 2 mezcladas con su alfa).
                // Interpolado da una orilla redondeada. No alcanza con contar
                // tiles: casi todos llevan pasto encima y lo pintado es apenas
                // el 57 % de su área; el agua se iría al doble del original.
                const float a = alpha(x, y);
                float painted = 0.0f;
                for (int ty = y - 1; ty <= y; ++ty)
                    for (int tx = x - 1; tx <= x; ++tx) {
                        if (tx < 0 || ty < 0) continue;
                        const int bits = tile[ty * 256 + tx];
                        painted += (bits & 1) * (1.0f - a) + ((bits >> 1) & 1) * a;
                    }
                o[2] = (unsigned char)(painted * 0.25f * 255.0f + 0.5f);
            }
            // Luz del terreno en el vértice (la que ilumina el agua original),
            // en luminancia y a la mitad, para que entre hasta 2.
            const float* l = &DAT_081cb608[size_t(i) * 3];
            const float lum = l[0] * 0.30f + l[1] * 0.55f + l[2] * 0.15f;
            o[3] = (unsigned char)(std::clamp(lum * 0.5f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    if (!any) return false;
    // Tiles de agua que quedan por encima de la superficie (paredes bajo los
    // puentes, barrancas empinadas): se dibujan con la textura más común de sus
    // vecinos y no se pintan como agua.
    std::fill(std::begin(wallTexture), std::end(wallTexture), (unsigned char)255);
    for (int ty = 0; ty < 255; ++ty)
        for (int tx = 0; tx < 255; ++tx) {
            const int c = ty * 256 + tx;
            if (!(tile[c] & 1)) continue;
            float surface = -1.0e9f;
            const int corners[4] = {c, c + 1, c + 256, c + 257};
            for (int k : corners)
                if (mask[size_t(k) * 4 + 1]) surface = (std::max)(surface, (mask[size_t(k) * 4 + 1] - 20) * 2.0f);
            const float high = (std::max)((std::max)(BackTerrainHeight[c], BackTerrainHeight[c + 1]),
                                          (std::max)(BackTerrainHeight[c + 256], BackTerrainHeight[c + 257]));
            // Redibujada: todo tile de agua toma el suelo de alrededor (lo que
            // se ve a través del agua nueva) y sigue contando como agua.
            const bool redraw = !allowDeep;
            if (!redraw && high <= surface + 15.0f) continue;
            int count[256] = {};
            int best = -1;
            for (int r = 1; r <= (redraw ? 4 : 1) && best < 0; ++r)
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) {
                        const int nx = tx + dx, ny = ty + dy;
                        if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx > 255 || ny > 255) continue;
                        const unsigned char t = TerrainMappingLayer1[ny * 256 + nx];
                        if (t == WaterTile || t == 255) continue;
                        if (++count[t] > (best < 0 ? 0 : count[best])) best = t;
                    }
            if (best < 0) continue;
            wallTexture[c] = (unsigned char)best;
            if (!redraw) mask[size_t(c) * 4] = (unsigned char)(((tile[c] & 2) + (mask[size_t(c) * 4] / 36 & 4)) * 36);
        }
    // El cauce se rearma sólo al cambiar de mapa (o de alturas).
    float sum = 0.0f;
    for (int i = 0; i < 256 * 256; i += 97) sum += BackTerrainHeight[i];
    if (map != carveMap || sum != carveSum) {
        carveMap = map;
        carveSum = sum;
        // Vértice de agua plena: sus cuatro tiles son agua y está a la altura
        // del agua (no es el borde de una pared ni de una barranca).
        std::vector<int> dist(256 * 256, 0);
        std::vector<int> queue;
        queue.reserve(256 * 256);
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                const int i = y * 256 + x;
                bool deep = x > 0 && y > 0 && x < 255 && y < 255;
                for (int ty = y - 1; deep && ty <= y; ++ty)
                    for (int tx = x - 1; tx <= x; ++tx)
                        if (!(tile[ty * 256 + tx] & 1) || tileLow[ty * 256 + tx] < BackTerrainHeight[i] - 3.0f) { deep = false; break; }
                dist[i] = deep ? 1 << 20 : 0;
                if (!deep) queue.push_back(i);
            }
        // Distancia (en tiles, 8 vecinos) a la orilla.
        for (size_t q = 0; q < queue.size(); ++q) {
            const int i = queue[q], x = i & 255, y = i >> 8;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx > 255 || ny > 255) continue;
                    const int n = ny * 256 + nx;
                    if (dist[n] > dist[i] + 1) { dist[n] = dist[i] + 1; queue.push_back(n); }
                }
        }
        for (int i = 0; i < 256 * 256; ++i) {
            const float t = std::clamp(dist[i] / CarveRamp, 0.0f, 1.0f);
            carve[i] = allowDeep ? CarveDepth * t * t * (3.0f - 2.0f * t) : 0.0f;
        }
    }
    carveActive = true;
    return true;
}
}
