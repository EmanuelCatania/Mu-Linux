#pragma once

// DESVIACION (iluminación mejorada, sólo WebGPU): cómo se ilumina cada mapa.
// Los valores por defecto están en la tabla de LightingProfiles.cpp y cada
// mapa los puede pisar desde Content/map/maps.toml:
//
//   [map.1.lighting]
//   sky = false
//   sun_shadow = 0.4
//
//   [[map.11.lighting.zone]]     # una parte del mapa con otro perfil
//   rect = [0, 76, 40, 110]      # casillas x0, y0, x1, y1 (o tiles = ["3"])
//   sky = false
//
// Claves: sky (bool), horizon / zenith / sun_color ([r, g, b]), clouds, rain,
// sun_shadow (cuánto se ve la sombra del sol, 1 = normal), shadow_softness
// (ancho del borde de la sombra, 1 = normal), y por modo de luz dinámica
// (conservador, contrastado): base, gain, radius y bloom ([a, b]). Atlans:
// underwater (bool), caustics, tint ([r, g, b]) y rays.
namespace LightingProfiles {

struct Look {
    float sky = 1.0f;                       // 1 = cielo; 0 = interior o bajo el agua
    float horizon[3] = {0.55f, 0.62f, 0.72f};
    float zenith[3] = {0.18f, 0.32f, 0.55f};
    float sunColor[3] = {1.0f, 0.88f, 0.70f};
    float clouds = 0.4f;                    // 0 = despejado, 1 = cubierto
    float rain = 0.0f;                      // nubes de tormenta
    float sunShadow = 1.0f;                 // multiplica la intensidad de la sombra del sol
    float shadowSoftness = 1.0f;            // multiplica el ancho del filtrado de la sombra
    // Por modo de luz dinámica: [0] conservador, [1] contrastado.
    float base[2] = {1.0f, 0.80f};          // multiplica la luz del 0.97k
    float gain[2] = {0.8f, 1.6f};           // intensidad de las luces por píxel
    float radius[2] = {1.15f, 1.5f};        // multiplica su alcance
    float bloom[2] = {0.7f, 1.0f};          // multiplica la intensidad del bloom
    // Bajo el agua (Atlans).
    float underwater = 0.0f;
    float caustics = 0.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    float rays = 0.0f;
};

// Perfil del mapa en la posición del héroe (tile y casilla). Al cambiar de
// zona o de mapa el resultado se funde con el anterior en medio segundo.
const Look& Current(int map, int heroTile, int gridX, int gridY);

}
