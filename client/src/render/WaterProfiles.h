#pragma once
#include "render/Gfx.h"

// DESVIACION (agua mejorada, sólo WebGPU): qué es agua en cada mapa y cómo se
// ve. El agua sale del propio mapa (los tiles con la textura 5 del terreno,
// que el motor ya trata como agua), así que un mapa nuevo la toma solo; la
// tabla de WaterProfiles.cpp sólo ajusta el color o la apaga donde no sirve.
namespace WaterProfiles {
// Llena la máscara (256x256 RGBA: por tile R = base agua (1) | capa 2 agua (2),
// en pasos de 85; por vértice G = toca un tile con agua, B = alfa de la capa 2,
// A = luz del terreno) y el aspecto del mapa.
// false si el mapa no lleva agua mejorada o no tiene agua.
bool Current(int map, unsigned char mask[256 * 256 * 4], Gfx::WaterLook& look);
}
