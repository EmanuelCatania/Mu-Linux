#pragma once
#include "render/Gfx.h"

// DESVIACION (agua mejorada, sólo WebGPU): qué es agua en cada mapa y cómo se
// ve. El agua sale del propio mapa (los tiles con la textura 5 del terreno,
// que el motor ya trata como agua), así que un mapa nuevo la toma solo; la
// tabla de WaterProfiles.cpp ajusta el color o la apaga donde esa textura no
// es agua (Dungeon, Lost Tower y Tarkan la usan para roca, lava y arena).
namespace WaterProfiles {
// Llena la máscara (256x256 RGBA: por tile R = base agua (1) | capa 2 agua (2)
// | cauce hondo (4), en pasos de 36; por vértice G = nivel del agua (z/2 + 20,
// 0 = sin agua), B = alfa de la capa 2, A = luz del terreno) y el aspecto del
// mapa.
// false si el mapa no lleva agua mejorada o no tiene agua.
bool Current(int map, unsigned char mask[256 * 256 * 4], Gfx::WaterLook& look);
// Cuánto se hunde cada vértice del terreno al dibujarlo para formar el cauce
// (256x256), o nullptr si el agua nueva no está activa en este mapa. Sólo es
// dibujo: las alturas del juego no cambian. Lo arma Current.
const float* Carve();
// Textura con que se dibuja cada tile en lugar del agua (255 = sin cambio), o
// nullptr si el agua nueva no está activa: los tiles que el mapa pinta con
// agua en una barranca o una pared y que la superficie no cubre toman la
// textura de sus vecinos, así no queda agua pintada en seco.
const unsigned char* WallTexture();
// Altura de la superficie si (x, y) está en un cauce hondo; -1e9 si no.
float DeepSurfaceAt(float x, float y);
void DisableCarve();
}
