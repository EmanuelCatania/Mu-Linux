// Capa Gfx, común a los backends. El estado del pipeline fijo (alpha test,
// niebla, texture env, matrices, color actual) se guarda en RenderState y lo
// aplican los shaders; lo que existe en el dispositivo va por GfxDevice.
// Antes de cada cambio se envía el lote pendiente, para no alterar el orden.
#include "render/Gfx.h"
#include "rhi/GfxDevice.h"
#include "render/BufferedDraw.h"
#include "render/RenderProfile.h"
#include "render/RenderState.h"
#include "render/TerrainDraw.h"
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace Gfx {
namespace {
bool mipmaps=false,bicubic=false;
// Wrap pedido por textura (S, T); sin entrada, el default de GL (repetir).
struct Wrap { bool s=true,t=true; };
std::unordered_map<Texture,Wrap> wraps;
void UpdateWrap() {
    auto& s=RenderState::Mutable();
    const auto it=wraps.find(s.texture);
    const Wrap w=it==wraps.end() ? Wrap{} : it->second;
    if (s.textureRepeat[0]==w.s && s.textureRepeat[1]==w.t) return;
    s.textureRepeat[0]=w.s;s.textureRepeat[1]=w.t;
    if (mipmaps || bicubic) RenderState::TouchMaterial();
}
bool MaterialCap(Cap cap) { return cap == Cap::Texture2D || cap == Cap::AlphaTest || cap == Cap::Fog; }
void CountCap(Cap cap) {
    if (cap == Cap::Blend) RenderProfile::Change(RenderProfile::State::Blend);
    if (cap == Cap::DepthTest) RenderProfile::Change(RenderProfile::State::Depth);
    if (cap == Cap::Texture2D) RenderProfile::Change(RenderProfile::State::Texture);
}
void SetCap(Cap cap, bool on) {
    auto& s = RenderState::Mutable();
    bool& current = s.caps[RenderState::CapIndex(cap)];
    if (current == on) return;
    BufferedDraw::Flush();
    current = on;CountCap(cap);
    GfxDevice::ApplyCap(cap, on);
    // El bicúbico sólo filtra lo 3D (con depth test): ese cap también es material.
    if (MaterialCap(cap) || ((mipmaps || bicubic) && cap == Cap::DepthTest)) RenderState::TouchMaterial();
}
void MaterialChange() { BufferedDraw::Flush();RenderState::TouchMaterial(); }
void MatrixChange() { BufferedDraw::Flush(); }
// Lleva al dispositivo lo que una restauración de atributos cambió.
void ApplyDevice(const RenderState::Fixed& before) {
    const auto& s = RenderState::Get();
    for (int i = 0; i < RenderState::CapCount; ++i) {
        if (before.caps[i] == s.caps[i]) continue;
        CountCap(static_cast<Cap>(i));
        GfxDevice::ApplyCap(static_cast<Cap>(i), s.caps[i]);
        if ((mipmaps || bicubic) && static_cast<Cap>(i) == Cap::DepthTest) RenderState::TouchMaterial();
    }
    if (before.blendSource != s.blendSource || before.blendDestination != s.blendDestination) {
        RenderProfile::Change(RenderProfile::State::Blend);GfxDevice::ApplyBlend(s.blendSource, s.blendDestination);
    }
    if (before.depthFunction != s.depthFunction) { RenderProfile::Change(RenderProfile::State::Depth);GfxDevice::ApplyDepthFunc(s.depthFunction); }
    if (before.depthWrite != s.depthWrite) { RenderProfile::Change(RenderProfile::State::Depth);GfxDevice::ApplyDepthMask(s.depthWrite); }
    if (before.texture != s.texture) { RenderProfile::Change(RenderProfile::State::Texture);GfxDevice::ApplyTexture(s.texture); }
    if (std::memcmp(before.viewport, s.viewport, sizeof(s.viewport)))
        GfxDevice::ApplyViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
}
}

void ResetState() {
    RenderState::Reset();
    GfxDevice::Reset();
}

void Enable(Cap cap) { SetCap(cap, true); }
void Disable(Cap cap) { SetCap(cap, false); }
bool IsEnabled(Cap cap) { return RenderState::Enabled(cap); }
void BlendFunc(Factor source, Factor destination) {
    auto& s = RenderState::Mutable();
    if (s.blendSource == source && s.blendDestination == destination) return;
    BufferedDraw::Flush();RenderProfile::Change(RenderProfile::State::Blend);
    s.blendSource = source;s.blendDestination = destination;
    GfxDevice::ApplyBlend(source, destination);
}
void DepthFunc(Compare function) {
    auto& s = RenderState::Mutable();
    if (s.depthFunction == function) return;
    BufferedDraw::Flush();RenderProfile::Change(RenderProfile::State::Depth);
    s.depthFunction = function;GfxDevice::ApplyDepthFunc(function);
}
void DepthMask(bool write) {
    auto& s = RenderState::Mutable();
    if (s.depthWrite == write) return;
    BufferedDraw::Flush();RenderProfile::Change(RenderProfile::State::Depth);
    s.depthWrite = write;GfxDevice::ApplyDepthMask(write);
}
void AlphaFunc(Compare function, float reference) {
    auto& s = RenderState::Mutable();
    if (s.alphaFunction == function && s.alphaReference == reference) return;
    MaterialChange();s.alphaFunction = function;s.alphaReference = reference;
}
void PushAttrib(unsigned attribMask) { BufferedDraw::Flush();RenderState::PushAttrib(attribMask); }
void PopAttrib() {
    BufferedDraw::Flush();
    RenderState::Fixed before;
    if (RenderState::PopAttrib(before)) ApplyDevice(before);
}

void Color3f(float r, float g, float b) { BufferedDraw::Color4f(r, g, b, 1.0f); }
void Color4f(float r, float g, float b, float a) { BufferedDraw::Color4f(r, g, b, a); }
void Color4fv(const float* rgba) { BufferedDraw::Color4f(rgba[0], rgba[1], rgba[2], rgba[3]); }
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a) { BufferedDraw::Color4ub(r, g, b, a); }
void CurrentColor(float rgba[4]) { std::memcpy(rgba, RenderState::Get().color, 4 * sizeof(float)); }

void MatrixMode(Matrix mode) { RenderState::SetMatrixMode(mode); }
void PushMatrix() { MatrixChange();RenderState::PushMatrix(); }
void PopMatrix() { MatrixChange();RenderState::PopMatrix(); }
void LoadIdentity() { MatrixChange();RenderState::LoadIdentity(); }
void Translate(float x, float y, float z) { MatrixChange();RenderState::Translate(x, y, z); }
void Rotate(float degrees, float x, float y, float z) { MatrixChange();RenderState::Rotate(degrees, x, y, z); }
void Ortho2D(double left, double right, double bottom, double top) { MatrixChange();RenderState::Ortho(left, right, bottom, top, -1, 1); }
void Perspective(double fovy, double aspect, double zNear, double zFar) { MatrixChange();RenderState::Perspective(fovy, aspect, zNear, zFar); }
void GetMatrix(Matrix which, float out[16]) { std::memcpy(out, RenderState::Top(which), 16 * sizeof(float)); }

void Viewport(int x, int y, int width, int height) {
    auto& v = RenderState::Mutable().viewport;
    if (v[0] == x && v[1] == y && v[2] == width && v[3] == height) return;
    BufferedDraw::Flush();v[0] = x;v[1] = y;v[2] = width;v[3] = height;
    GfxDevice::ApplyViewport(x, y, width, height);
}
void ClearColor(float r, float g, float b, float a) { BufferedDraw::Flush();GfxDevice::ClearColor(r, g, b, a); }
void Clear(unsigned clearMask) { BufferedDraw::Flush();GfxDevice::Clear(clearMask); }
void Flush() { BufferedDraw::Flush();GfxDevice::Flush(); }
void ClearErrors() { RenderState::TakeStackError();GfxDevice::ClearErrors(); }
void PostProcess(bool antialias, float sharpen) {
    if (!antialias && sharpen <= 0.0f) return;
    BufferedDraw::Flush();
    GfxDevice::PostProcess(antialias, sharpen);
}
void ReadPixelsRGB(int x, int y, int width, int height, void* out) {
    BufferedDraw::Flush();
    GfxDevice::ReadPixelsRGB(x, y, width, height, out);
}
int MaxTextureSize() { return GfxDevice::MaxTextureSize(); }
void MaxViewportDims(int out[2]) { GfxDevice::MaxViewportDims(out); }
const char* GetDeviceInfo(DeviceInfo info) { return GfxDevice::GetDeviceInfo(info); }

Texture CreateTexture() { return GfxDevice::CreateTexture(); }
void DeleteTexture(Texture texture) {
    wraps.erase(texture);
    BufferedDraw::Flush();TerrainDraw::InvalidateTexture(texture);RenderProfile::ForgetTexture(texture);
    auto& s = RenderState::Mutable();if (s.texture == texture) s.texture = 0;
    GfxDevice::DeleteTexture(texture);
}
void BindTexture(Texture texture) {
    auto& s = RenderState::Mutable();
    if (s.texture == texture) return;
    BufferedDraw::Flush();RenderProfile::Change(RenderProfile::State::Texture);
    s.texture = texture;GfxDevice::ApplyTexture(texture);
    UpdateWrap();
}
void TexImage2D(int components, int width, int height, Format format, const void* pixels) {
    BufferedDraw::Flush();TerrainDraw::InvalidateBoundTexture();
    GfxDevice::TexImage2D(components, width, height, format, pixels);
}
void TexSubImage2D(int x, int y, int width, int height, Format format, const void* pixels) {
    BufferedDraw::Flush();TerrainDraw::InvalidateBoundTexture();
    GfxDevice::TexSubImage2D(x, y, width, height, format, pixels);
}
void TexParameter(TexParam parameter, TexValue value) {
    BufferedDraw::Flush();TerrainDraw::InvalidateBoundTexture();
    GfxDevice::TexParameter(parameter, value);
    if (parameter == TexParam::WrapS || parameter == TexParam::WrapT) {
        auto& w=wraps[RenderState::Get().texture];
        (parameter == TexParam::WrapS ? w.s : w.t) = value == TexValue::Repeat;
        UpdateWrap();
    }
}
void SetTextureQuality(bool useMipmaps, bool useBicubic) {
    if (mipmaps==useMipmaps && bicubic==useBicubic) return;
    BufferedDraw::Flush();mipmaps=useMipmaps;bicubic=useBicubic;RenderState::TouchMaterial();
}
bool TextureMipmaps() { return mipmaps; }
bool TextureBicubic() { return bicubic; }
namespace { int multisample = 0; }
void SetMultisample(int samples) { multisample = samples; }
int Multisample() { return multisample; }
namespace { bool shadowMaps = false, depthFog = false; float shadowStrength = 0.5f; bool shadowScene = false; }
void SetShadowMaps(bool enabled, float strength) { shadowMaps = enabled; shadowStrength = strength; }
bool ShadowMaps() { return shadowMaps && GfxDevice::SupportsShadowMaps(); }
void SetDepthFog(bool enabled) { depthFog = enabled; }
bool DepthFog() { return depthFog && GfxDevice::SupportsShadowMaps(); }
void BeginShadowScene(const float focus[3]) {
    if ((!ShadowMaps() && !DepthFog()) || shadowScene) return;
    BufferedDraw::Flush();
    shadowScene = true;
    // Sin sombras la escena sólo sirve para la niebla: intensidad 0.
    GfxDevice::ShadowScene(true, focus, ShadowMaps() ? shadowStrength : 0.0f);
}
void Atmosphere(int mode, const float color[3], float density, float start) {
    if (!shadowScene) return;
    GfxDevice::Atmosphere(DepthFog() ? mode : 0, color, density, start);
}
namespace {
bool water = true;
// Luces que se reflejan en el próximo Water(): posición y color, 8 floats c/u.
constexpr int MaxWaterLights = 8;
float waterLights[MaxWaterLights * 8];
int waterLightCount = 0;
// Barcos que levantan olas de proa: proa (x, y) y rumbo (x, y).
constexpr int MaxWaterShips = 4;
float waterShips[MaxWaterShips * 4];
int waterShipCount = 0;
}
void ToggleWater() { water = !water; }
bool WaterEnabled() { return water && GfxDevice::SupportsShadowMaps(); }
void WaterLight(const float position[3], const float color[3], bool compact) {
    if (waterLightCount >= MaxWaterLights) return;
    // Los efectos pueden pasar dos veces por cuadro (tick y dibujo).
    for (int i = 0; i < waterLightCount; ++i) {
        const float* o = waterLights + i * 8;
        if (std::fabs(o[0] - position[0]) < 1.0f && std::fabs(o[1] - position[1]) < 1.0f && std::fabs(o[2] - position[2]) < 1.0f) return;
    }
    float* l = waterLights + waterLightCount++ * 8;
    l[0] = position[0]; l[1] = position[1]; l[2] = position[2]; l[3] = 1.0f;
    l[4] = color[0]; l[5] = color[1]; l[6] = color[2]; l[7] = compact ? 1.0f : 0.0f;
}
void WaterShip(const float bow[2], const float heading[2]) {
    if (waterShipCount >= MaxWaterShips) return;
    float* s = waterShips + waterShipCount++ * 4;
    s[0] = bow[0]; s[1] = bow[1]; s[2] = heading[0]; s[3] = heading[1];
}
float WaterSwell(float x, float y, float time, float* slopeX, float* slopeY) {
    // Igual que la primera onda de slope() en el shader del agua (WebGPU).
    const float dl = std::sqrt(0.35f * 0.35f + 0.94f * 0.94f);
    const float dx = 0.35f / dl, dy = 0.94f / dl;
    const float k = 6.2831853f / 900.0f;
    const float omega = std::sqrt(300.0f * k);
    const float maxSlope = 0.035f;
    const float phase = k * (dx * x + dy * y) - omega * time;
    if (slopeX) *slopeX = dx * maxSlope * std::cos(phase);
    if (slopeY) *slopeY = dy * maxSlope * std::cos(phase);
    return maxSlope / k * std::sin(phase);
}
void Water(float level, float time, const float sun[3], float backdropY) {
    const int count = waterLightCount;
    waterLightCount = 0;
    const int ships = waterShipCount;
    waterShipCount = 0;
    if (!WaterEnabled()) return;
    BufferedDraw::Flush();
    // [0..7] parámetros, [8] cantidad de luces, [12..] las luces.
    // [9] cantidad de barcos, [12 + 64..] los barcos.
    float params[12 + MaxWaterLights * 8 + MaxWaterShips * 4] = {level, time, sun[0], sun[1], sun[2], backdropY, 0.0f, 0.0f, (float)count, (float)ships};
    for (int i = 0; i < count * 8; ++i) params[12 + i] = waterLights[i];
    for (int i = 0; i < ships * 4; ++i) params[12 + MaxWaterLights * 8 + i] = waterShips[i];
    GfxDevice::Water(params);
}
void SetShadowCasting(bool enabled) {
    if (!shadowScene) return;
    BufferedDraw::Flush();
    GfxDevice::ShadowCasting(enabled);
}
void EndShadowScene() {
    if (!shadowScene) return;
    BufferedDraw::Flush();
    shadowScene = false;
    GfxDevice::ShadowScene(false, nullptr, 0.0f);
}
namespace { bool hdTextures = false; }
void SetHDTextures(bool enabled) { hdTextures = enabled; }
bool HDTextures() { return hdTextures; }
// Valores numéricos de OpenGL que trae el binario (p. ej. OpenJPG(..., 0x2601, 0x2901)).
TexValue TexValueFromLegacy(int value) {
    switch (value) {
    case 0x2601: return TexValue::Linear;               // GL_LINEAR
    case 0x2700: return TexValue::NearestMipmapNearest; // GL_NEAREST_MIPMAP_NEAREST
    case 0x2701: return TexValue::LinearMipmapNearest;  // GL_LINEAR_MIPMAP_NEAREST
    case 0x2702: return TexValue::NearestMipmapLinear;  // GL_NEAREST_MIPMAP_LINEAR
    case 0x2703: return TexValue::LinearMipmapLinear;   // GL_LINEAR_MIPMAP_LINEAR
    case 0x2901: return TexValue::Repeat;               // GL_REPEAT
    case 0x2900: return TexValue::Clamp;                // GL_CLAMP
    case 0x812F: return TexValue::ClampToEdge;          // GL_CLAMP_TO_EDGE
    default: return TexValue::Nearest;
    }
}
void TexBorderColor(const float rgba[4]) {
    BufferedDraw::Flush();TerrainDraw::InvalidateBoundTexture();
    GfxDevice::TexBorderColor(rgba);
}
void TexEnvMode(TexEnv mode) {
    auto& s = RenderState::Mutable();
    if (s.texEnv == mode) return;
    MaterialChange();s.texEnv = mode;
}
void SetUnpackAlignment(int alignment) { BufferedDraw::Flush();GfxDevice::SetUnpackAlignment(alignment); }
int UnpackAlignment() { return GfxDevice::UnpackAlignment(); }

void SetFogMode(FogMode mode) { auto& s = RenderState::Mutable();if (s.fogMode == mode) return;MaterialChange();s.fogMode = mode; }
void FogStart(float value) { auto& s = RenderState::Mutable();if (s.fogStart == value) return;MaterialChange();s.fogStart = value; }
void FogEnd(float value) { auto& s = RenderState::Mutable();if (s.fogEnd == value) return;MaterialChange();s.fogEnd = value; }
void FogDensity(float value) { auto& s = RenderState::Mutable();if (s.fogDensity == value) return;MaterialChange();s.fogDensity = value; }
void FogColor(const float rgba[4]) {
    auto& s = RenderState::Mutable();
    if (!std::memcmp(s.fogColor, rgba, sizeof(s.fogColor))) return;
    MaterialChange();std::memcpy(s.fogColor, rgba, sizeof(s.fogColor));
}

}
