// Backend WebGPU de GfxDevice y BufferedDraw.
//
// Cada lote (Submit) guarda sus vértices, una foto del estado de RenderState
// (pipeline, uniforms, textura, viewport) y el dibujo. Gfx::Clear abre una
// pasada nueva. En Present se sube todo de una vez y se codifica en un único
// envío: así un buffer o una textura que se reescribe a mitad de cuadro no
// cambia los dibujos ya grabados (una textura usada en el cuadro que se vuelve
// a escribir pasa a una copia nueva; los dibujos anteriores conservan la vieja).
#include "rhi/Api.h"
#include "rhi/GfxDevice.h"
#include "rhi/WebGPU/Device.h"
#include "rhi/WebGPU/Renderer.h"
#include "render/MeshTopology.h"
#include "render/RenderState.h"
#include "render/Skinning.h"
#include "render/ShadowLight.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" void DbgLogPublic(const char*);

namespace WebGPU {
namespace {
using Gfx::Cap;
using BufferedDraw::Vertex;

// Modos de BufferedDraw (numeración de OpenGL 1.1). Abanicos, polígonos y
// quad strips ya llegan convertidos.
enum Mode : unsigned { Points=0, Lines=1, LineLoop=2, LineStrip=3, Triangles=4, TriangleStrip=5, Quads=7 };

const char* const Shader = R"WGSL(
struct Uniforms {
    mvp: mat4x4f, modelView: mat4x4f, textureMatrix: mat4x4f,
    flags: vec4i,     // textured, texEnv, alphaFunction, fog
    params: vec4f,    // alphaReference, fogStart, fogEnd, fogDensity
    fogColor: vec4f,
    extra: vec4i,     // bicúbico, recibe sombra, recibe luz (iluminación mejorada)
};
// Datos por dibujo en tablas: cada dibujo lleva su índice en instance_index
// (firstInstance) a drawTable, que da su fila de uniforms y de skin. El grupo 0
// se asocia una vez por pasada en lugar de una vez por dibujo.
@group(0) @binding(0) var<storage, read> uniformTable: array<Uniforms>;
@group(0) @binding(3) var<storage, read> drawTable: array<vec2u>;
var<private> u: Uniforms;
@group(1) @binding(0) var tex: texture_2d<f32>;
@group(1) @binding(1) var smp: sampler;
// Sombras proyectadas: params = activa, intensidad, tamaño del texel, sesgo.
// Iluminación mejorada: lighting = luces, intensidad, luz base; fx =
// cáusticas, tiempo; lights = por luz posición + alcance, color.
struct Shadow { lightViewProj: mat4x4f, inverseView: mat4x4f, params: vec4f,
                lighting: vec4f, fx: vec4f, lights: array<vec4f, 128> };
@group(2) @binding(0) var<uniform> shadow: Shadow;
@group(2) @binding(1) var shadowMap: texture_depth_2d;
@group(2) @binding(2) var shadowSampler: sampler_comparison;
struct VertexOut {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec4f,
    @location(2) fogDistance: f32,
    @location(3) world: vec3f,
    @location(4) @interpolate(flat) slot: u32,   // fila de uniformTable
    @location(5) normal: vec3f,                  // en el mundo; 0 = sacarla de la superficie
};
// Posición en el mundo: la cámara de la escena deshace la vista del dibujo.
fn worldOf(position: vec4f) -> vec3f {
    return (shadow.inverseView * (u.modelView * position)).xyz;
}
@vertex fn vs(@location(0) p: vec3f, @location(1) color: vec4f, @location(2) uv: vec4f, @builtin(instance_index) id: u32) -> VertexOut {
    var out: VertexOut;
    let ids = drawTable[id]; u = uniformTable[ids.x]; out.slot = ids.x;
    let position = vec4f(p, 1.0);
    var clip = u.mvp * position;
    // Las matrices son de OpenGL (z de -1 a 1); WebGPU usa z de 0 a 1.
    clip.z = (clip.z + clip.w) * 0.5;
    out.position = clip;
    out.color = clamp(color, vec4f(0.0), vec4f(1.0));
    out.uv = u.textureMatrix * uv;
    out.fogDistance = abs((u.modelView * position).z);
    out.world = worldOf(position);
    out.normal = vec3f(0.0);
    return out;
}
// Mallas residentes: posición y normal en el espacio del hueso; la paleta
// tiene tres filas por hueso (rotación en xyz, traslación en w).
struct Skin {
    paletteBase: i32, flags: u32, pad0: i32, pad1: i32,   // flags: luz, sombra, textura
    scales: vec4f, origin: vec4f, direction: vec4f, coefficients: vec4f,
    bodyLight: vec4f, uvOffset: vec4f, shadowOrigin: vec4f, color: vec4f, uv: vec4f,
    chromeU: vec4f, chromeV: vec4f,   // flags & 8: UV chrome lineal en la normal rotada
    pad2: vec4f, pad3: vec4f, pad4: vec4f, pad5: vec4f,   // fila de 256 bytes
};
@group(0) @binding(1) var<storage, read> skinTable: array<Skin>;
var<private> skin: Skin;
@group(0) @binding(2) var<storage, read> palette: array<vec4f>;
fn rotateBone(p: vec3f, b: i32) -> vec3f {
    let r = skin.paletteBase + b * 3;
    return vec3f(dot(palette[r].xyz, p), dot(palette[r + 1].xyz, p), dot(palette[r + 2].xyz, p));
}
// UV de las pasadas chrome/oil/obj (BMD__RenderMesh): lineal en la normal
// rotada por el hueso, la misma que usa la ruta CPU.
fn chromeUV(normal: vec4f) -> vec4f {
    let n = rotateBone(normal.xyz, i32(normal.w));
    return vec4f(dot(skin.chromeU.xyz, n) + skin.chromeU.w, dot(skin.chromeV.xyz, n) + skin.chromeV.w, 0.0, 1.0);
}
fn skinPosition(p0: vec3f, b: i32) -> vec3f {
    let r = skin.paletteBase + b * 3;
    var p = rotateBone(p0, b) * skin.scales.x;
    p = p + vec3f(palette[r].w, palette[r + 1].w, palette[r + 2].w);
    return p * skin.scales.y + skin.origin.xyz;
}
@vertex fn vs_skin(@location(0) p0: vec3f, @location(1) bone: f32, @location(2) normal: vec4f, @location(3) uv2: vec2f, @builtin(instance_index) id: u32) -> VertexOut {
    var out: VertexOut;
    let ids = drawTable[id]; u = uniformTable[ids.x]; skin = skinTable[ids.y]; out.slot = ids.x;
    var p = skinPosition(p0, i32(bone));
    if ((skin.flags & 2u) != 0u) {
        // Sombra proyectada de BMD__RenderBodyShadow.
        let l = p - skin.shadowOrigin.xyz;
        p = vec3f(l.x + (l.x + 2000.0) * l.z / (l.z - 4000.0) + skin.shadowOrigin.x, l.y + skin.shadowOrigin.y, skin.shadowOrigin.z + 5.0);
    }
    let position = vec4f(p, 1.0);
    var clip = u.mvp * position;
    clip.z = (clip.z + clip.w) * 0.5;
    out.position = clip;
    var color = skin.color;
    if ((skin.flags & 1u) != 0u) {
        let n = rotateBone(normal.xyz, i32(normal.w));
        let light = max((skin.direction.y * n.y + skin.direction.x * n.x + skin.direction.z * n.z) * skin.coefficients.x + skin.coefficients.y, skin.coefficients.z);
        color = vec4f(light * skin.bodyLight.xyz, color.a);
    }
    var uv = skin.uv;
    if ((skin.flags & 4u) != 0u) { uv = vec4f(uv2, 0.0, 1.0); }
    if ((skin.flags & 8u) != 0u) { uv = chromeUV(normal); }
    out.color = clamp(color, vec4f(0.0), vec4f(1.0));
    out.uv = u.textureMatrix * (uv + vec4f(skin.uvOffset.xy, 0.0, 0.0));
    out.fogDistance = abs((u.modelView * position).z);
    out.world = worldOf(position);
    out.normal = (shadow.inverseView * (u.modelView * vec4f(rotateBone(normal.xyz, i32(normal.w)), 0.0))).xyz;
    return out;
}
// Pase de sombras: los mismos dibujos vistos desde la luz, sólo profundidad.
struct CasterOut {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec4f,
    @location(2) @interpolate(flat) slot: u32,
};
@vertex fn vs_caster(@location(0) p: vec3f, @location(1) color: vec4f, @location(2) uv: vec4f, @builtin(instance_index) id: u32) -> CasterOut {
    var out: CasterOut;
    let ids = drawTable[id]; u = uniformTable[ids.x]; out.slot = ids.x;
    out.position = shadow.lightViewProj * vec4f(worldOf(vec4f(p, 1.0)), 1.0);
    out.color = clamp(color, vec4f(0.0), vec4f(1.0));
    out.uv = u.textureMatrix * uv;
    return out;
}
@vertex fn vs_skin_caster(@location(0) p0: vec3f, @location(1) bone: f32, @location(2) normal: vec4f, @location(3) uv2: vec2f, @builtin(instance_index) id: u32) -> CasterOut {
    var out: CasterOut;
    let ids = drawTable[id]; u = uniformTable[ids.x]; skin = skinTable[ids.y]; out.slot = ids.x;
    let p = skinPosition(p0, i32(bone));
    out.position = shadow.lightViewProj * vec4f(worldOf(vec4f(p, 1.0)), 1.0);
    out.color = clamp(skin.color, vec4f(0.0), vec4f(1.0));
    var uvs = skin.uv;
    if ((skin.flags & 4u) != 0u) { uvs = vec4f(uv2, 0.0, 1.0); }
    if ((skin.flags & 8u) != 0u) { uvs = chromeUV(normal); }
    out.uv = u.textureMatrix * (uvs + vec4f(skin.uvOffset.xy, 0.0, 0.0));
    return out;
}
// No proyecta lo que se dibuja semitransparente por su color (un techo que
// se desvanece al entrar a un edificio, un objeto que aparece) ni lo
// transparente de la textura (hojas, rejas).
@fragment fn fs_caster(in: CasterOut) {
    u = uniformTable[in.slot];
    let t = textureSample(tex, smp, in.uv.xy / in.uv.w);
    if (in.color.a < 0.99) { discard; }
    if (u.flags.x != 0 && t.a < 0.25) { discard; }
}
// Fracción iluminada (PCF 3x3), con borde suave al final del mapa.
fn shadowFactor(world: vec3f) -> f32 {
    let lp = shadow.lightViewProj * vec4f(world, 1.0);
    let uv = vec2f(lp.x * 0.5 + 0.5, 0.5 - lp.y * 0.5);
    if (any(uv < vec2f(0.0)) || any(uv > vec2f(1.0)) || lp.z > 1.0) { return 1.0; }
    var lit = 0.0;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            lit += textureSampleCompareLevel(shadowMap, shadowSampler, uv + vec2f(f32(x), f32(y)) * shadow.params.z, lp.z - shadow.params.w);
        }
    }
    lit = lit / 9.0;
    let edge = smoothstep(0.0, 0.1, min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y)));
    return mix(1.0, mix(1.0 - shadow.params.y, 1.0, lit), edge);
}
// Catmull-Rom con los 16 texels del nivel 0 (textureLoad: el 0.97k usa casi
// siempre filtro nearest, así que el sampler no sirve para interpolar).
fn wrapTexel(c: i32, size: i32, repeat: bool) -> i32 {
    if (repeat) { return ((c % size) + size) % size; }
    return clamp(c, 0, size - 1);
}
fn cubicWeights(f: f32) -> vec4f {
    return vec4f(f * (-0.5 + f * (1.0 - 0.5 * f)), 1.0 + f * f * (-2.5 + 1.5 * f),
                 f * (0.5 + f * (2.0 - 1.5 * f)), f * f * (-0.5 + 0.5 * f));
}
fn catmullRom(uv: vec2f, size: vec2f) -> vec4f {
    let isize = vec2i(size);
    let p = uv * size - 0.5;
    let i = floor(p);
    let f = p - i;
    let wx = cubicWeights(f.x);
    let wy = cubicWeights(f.y);
    let repeatS = (u.extra.x & 2) != 0;
    let repeatT = (u.extra.x & 4) != 0;
    var r = vec4f(0.0);
    for (var y = 0; y < 4; y++) {
        let ty = wrapTexel(i32(i.y) + y - 1, isize.y, repeatT);
        var row = vec4f(0.0);
        for (var x = 0; x < 4; x++) {
            let tx = wrapTexel(i32(i.x) + x - 1, isize.x, repeatS);
            row += textureLoad(tex, vec2i(tx, ty), 0) * wx[x];
        }
        r += row * wy[y];
    }
    return clamp(r, vec4f(0.0), vec4f(1.0));
}
// Cáusticas: el reflejo de la luz que pasa por las olas, sobre el fondo. El
// patrón se repite cada 1 en uv (sin costura).
fn caustic(uv: vec2f, time: f32) -> f32 {
    let tau = 6.28318;
    let p = (fract(uv) * tau) - 250.0;
    var i = p;
    var c = 1.0;
    for (var n = 0; n < 4; n++) {
        let t = time * (1.0 - 3.5 / f32(n + 1));
        i = p + vec2f(cos(t - i.x) + sin(t + i.y), sin(t - i.y) + cos(t + i.x));
        c += 1.0 / length(vec2f(p.x / (sin(i.x + t) / 0.005), p.y / (cos(i.y + t) / 0.005)));
    }
    c = 1.17 - pow(c / 4.0, 1.4);
    return clamp(pow(abs(c), 8.0), 0.0, 1.5);
}
// Luces puntuales del mundo y cáusticas sobre lo que recibe luz. albedo es el
// color de la textura; normal, la de la malla o la de la superficie.
fn sceneLight(c: vec3f, albedo: vec3f, world: vec3f, normal: vec3f) -> vec3f {
    let camera = shadow.inverseView[3].xyz;
    var n = normal;
    if (dot(n, camera - world) < 0.0) { n = -n; }
    var add = vec3f(0.0);
    let count = i32(shadow.lighting.x);
    for (var i = 0; i < count; i++) {
        let l = shadow.lights[i * 2];
        let d = l.xyz - world;
        let r = l.w;
        let dist2 = dot(d, d);
        if (dist2 >= r * r) { continue; }
        let x = dist2 / (r * r);
        let falloff = (1.0 - x) * (1.0 - x);
        // Luz envolvente: los lados que no miran a la luz no quedan negros.
        let facing = dot(n, d * inverseSqrt(max(dist2, 1.0))) * 0.6 + 0.4;
        add += shadow.lights[i * 2 + 1].rgb * falloff * max(facing, 0.0);
    }
    // Tope suave: muchas luces juntas (una lluvia de flechas, un skill) no
    // queman la imagen a blanco.
    let boost = add * shadow.lighting.y;
    var result = c * shadow.lighting.z + albedo * boost / (vec3f(1.0) + boost * 0.7);
    if (shadow.fx.x > 0.0) {
        // Más fuerte en lo que mira para arriba; dos escalas que se mueven distinto.
        let up = clamp(n.z, 0.0, 1.0);
        let k = caustic(world.xy / 520.0, shadow.fx.y * 0.6) * 0.6 + caustic(world.xy / 310.0 + 0.37, shadow.fx.y * 0.45) * 0.4;
        result += albedo * vec3f(0.55, 0.85, 1.0) * k * up * shadow.fx.x;
    }
    return result;
}
@fragment fn fs(in: VertexOut) -> @location(0) vec4f {
    u = uniformTable[in.slot];
    // Normal de la superficie desde las derivadas (control uniforme: va antes de todo).
    let faceNormal = cross(dpdx(in.world), dpdy(in.world));
    // textureProj: divide por q. Se muestrea siempre (control uniforme).
    let uv = in.uv.xy / in.uv.w;
    var t = textureSample(tex, smp, uv);
    // DESVIACION: bicúbico (opción de calidad), sólo al ampliar la textura.
    let size = vec2f(textureDimensions(tex, 0));
    // Texels por píxel en el eje mayor (como el LOD del hardware, no fwidth).
    let st = uv * size;
    let footprint = max(length(dpdx(st)), length(dpdy(st)));
    if (u.extra.x != 0 && footprint < 1.0) { t = catmullRom(uv, size); }
    var c = in.color;
    if (u.flags.x != 0) {
        let env = u.flags.y;
        if (env == 1) { c = t; }
        else if (env == 2) { c = vec4f(mix(in.color.rgb, t.rgb, t.a), in.color.a); }
        else if (env == 4) { c = vec4f(in.color.rgb + t.rgb, in.color.a * t.a); }
        else { c = t * in.color; }
    }
    let f = u.flags.z;
    if (f != 0) {
        let a = c.a;
        let r = u.params.x;
        var passed = true;
        if (f == 1) { passed = false; }
        else if (f == 2) { passed = a < r; }
        else if (f == 3) { passed = a == r; }
        else if (f == 4) { passed = a <= r; }
        else if (f == 5) { passed = a > r; }
        else if (f == 6) { passed = a != r; }
        else if (f == 7) { passed = a >= r; }
        if (!passed) { discard; }
    }
    // DESVIACION: sombras proyectadas (opción), antes de la niebla.
    if (u.extra.y != 0 && shadow.params.x > 0.5) {
        c = vec4f(c.rgb * shadowFactor(in.world), c.a);
    }
    // DESVIACION: iluminación mejorada (luces por píxel, cáusticas).
    if (u.extra.z != 0 && (shadow.lighting.x > 0.5 || shadow.fx.x > 0.0 || shadow.lighting.z != 1.0)) {
        var n = in.normal;
        if (dot(n, n) < 1e-6) { n = faceNormal; }
        n = n * inverseSqrt(max(dot(n, n), 1e-12));
        let albedo = select(c.rgb, t.rgb, u.flags.x != 0);
        c = vec4f(sceneLight(c.rgb, albedo, in.world, n), c.a);
    }
    let fog = u.flags.w;
    if (fog != 0) {
        let d = in.fogDistance;
        var k = 1.0;
        if (fog == 1) {
            if (u.params.z == u.params.y) { k = select(0.0, 1.0, d <= u.params.z); }
            else { k = (u.params.z - d) / (u.params.z - u.params.y); }
        }
        if (fog == 2) { k = exp(-u.params.w * d); }
        if (fog == 3) { let v = u.params.w * d; k = exp(-v * v); }
        c = vec4f(mix(u.fogColor.rgb, c.rgb, clamp(k, 0.0, 1.0)), c.a);
    }
    return c;
}
)WGSL";

struct Uniforms {
    float mvp[16], modelView[16], textureMatrix[16];
    int32_t flags[4];
    float params[4];
    float fogColor[4];
    int32_t extra[4];
};
struct Skin {
    int32_t paletteBase; uint32_t flags; int32_t pad[2];
    float scales[4], origin[4], direction[4], coefficients[4], bodyLight[4], uvOffset[4], shadowOrigin[4], color[4], uv[4];
    float chromeU[4], chromeV[4];
};
struct ShadowUniform {
    float lightViewProj[16], inverseView[16];
    float params[4];   // activa, intensidad, tamaño del texel, sesgo
    float lighting[4]; // iluminación mejorada: luces, intensidad, luz base
    float fx[4];       // cáusticas, tiempo
    float lights[128][4];   // por luz: posición + alcance, color
};
constexpr uint32_t ShadowSize = ShadowLight::Size;
constexpr uint64_t PostUniformSize = 80;   // struct Post del shader del posproceso
constexpr uint64_t UniformStride = 256;
static_assert(sizeof(Skin) <= UniformStride, "Skin no entra en su tramo");
static_assert(sizeof(Uniforms) <= UniformStride, "Uniforms no entra en su tramo");

// ── Texturas ────────────────────────────────────────────────────────────────
struct Sampling {
    Gfx::TexValue minFilter = Gfx::TexValue::NearestMipmapLinear, magFilter = Gfx::TexValue::Linear;
    Gfx::TexValue wrapS = Gfx::TexValue::Repeat, wrapT = Gfx::TexValue::Repeat;
    uint32_t Key() const {
        return uint32_t(minFilter) | uint32_t(magFilter) << 8 | uint32_t(wrapS) << 16 | uint32_t(wrapT) << 24;
    }
};
struct TextureSlot {
    WGPUTexture texture = nullptr;
    WGPUTextureView view = nullptr;
    int width = 0, height = 0;
    Sampling sampling;
    // Bind groups (vista + sampler) por combinación de filtro y wrap: el
    // cliente vuelve a fijar el filtro al ligar la textura, y alternar entre
    // dos no debe crear un bind group por dibujo.
    struct Group { uint32_t key; WGPUBindGroup group; };
    std::vector<Group> groups;
    uint64_t usedFrame = 0;          // último cuadro grabado que la usa
};

// ── Grabación del cuadro ────────────────────────────────────────────────────
// Clase de vértice del pipeline, en los bits altos de PipelineKey::topology.
enum Kind : uint8_t { Immediate = 0, Resident = 1 };
struct PipelineKey {
    uint8_t topology = 0, blend = 0, src = 0, dst = 0, depthTest = 0, depthFunc = 0, depthWrite = 0, cull = 0;
    uint64_t Value() const {
        uint64_t v = 0;
        std::memcpy(&v, this, sizeof(*this));
        return v;
    }
};
static_assert(sizeof(PipelineKey) == 8, "PipelineKey ocupa 8 bytes");
struct Draw {
    WGPURenderPipeline pipeline;
    WGPUBindGroup textureGroup;
    uint32_t uniformOffset, skinOffset;
    uint32_t firstVertex, vertexCount;
    bool quads;
    bool caster;   // dentro de la escena de sombras: se dibuja también desde la luz
    bool overWater;   // objeto del mapa aditivo: Water lo pasa encima del agua redibujada
    uint8_t topology;   // WGPUPrimitiveTopology del dibujo inmediato
    int viewport[4];
    // Malla residente: sus propios buffers (nulos en el dibujo inmediato).
    WGPUBuffer vertexBuffer, indexBuffer;
    uint32_t indexCount;
    uint32_t id;   // fila de drawTable; se asigna al codificar (firstInstance)
};
// Buffers de una malla BMD residente; se rehacen al cambiar su topología.
struct GpuMesh {
    WGPUBuffer indices = nullptr, vertices = nullptr;
    uint64_t revision = 0, residentRevision = 0, usedFrame = 0;
};
// Buffer de staging mapeable que se reusa entre cuadros. Las subidas grandes de
// cada cuadro (vértices, uniforms, skins, paleta, tabla de dibujos) se escriben
// en él y se copian a los buffers de la GPU dentro del mismo encoder. Con
// wgpuQueueWriteBuffer, Dawn creaba y destruía un staging propio por subida.
struct Staging {
    WGPUBuffer buffer = nullptr;
    uint64_t size = 0;
    uint8_t* mapped = nullptr;   // no nulo: mapeado y libre para el cuadro
    bool orphan = false;         // el renderer se liberó con el mapeo pendiente
};
struct Pass {
    bool clearColor = false, clearDepth = false;
    // Posproceso sobre postViewport antes de la pasada: FXAA y nitidez (> 0).
    bool antialias = false;
    float sharpen = 0.0f;
    // Efectos de cámara (Gfx::PostEffects): [0] bajo el agua, [1] tiempo,
    // [2] ondulación, [3] aberración, [4..6] tinte, [7] viñeteado, [8] rayos,
    // [9] bloom, [10] umbral del bloom.
    float fx[12]{};
    int postViewport[4]{};
    bool Underwater() const { return fx[0] > 0.0f; }
    bool Bloom() const { return fx[9] > 0.0f; }
    bool Post() const { return antialias || sharpen > 0.0f || Underwater() || Bloom(); }
    bool atmosphere = false;   // niebla por profundidad antes de los dibujos de la pasada
    bool water = false;        // mar (prueba) antes de los dibujos de la pasada
    float color[4]{};
    std::vector<Draw> draws;
};

struct State {
    bool ready = false;
    WGPUShaderModule module = nullptr;
    WGPUBindGroupLayout uniformLayout = nullptr, textureLayout = nullptr;
    WGPUPipelineLayout pipelineLayout = nullptr;
    WGPUBuffer vertexBuffer = nullptr, uniformBuffer = nullptr, indexBuffer = nullptr;
    uint64_t vertexCapacity = 0, uniformCapacity = 0, indexQuads = 0;
    WGPUBindGroup uniformGroup = nullptr;
    WGPUBuffer skinBuffer = nullptr, paletteBuffer = nullptr, drawBuffer = nullptr;
    uint64_t skinCapacity = 0, paletteCapacity = 0, drawCapacity = 0;
    std::vector<uint32_t> drawTable;   // por dibujo: fila de uniforms y de skin
    std::unordered_map<const void*, GpuMesh> meshes;
    bool residentEnabled = true;
    WGPUTexture depth = nullptr;
    WGPUTextureView depthView = nullptr;
    unsigned depthWidth = 0, depthHeight = 0;
    // MSAA: color multisample que cada pasada resuelve sobre la textura del cuadro.
    WGPUTexture msaa = nullptr;
    WGPUTextureView msaaView = nullptr;
    unsigned msaaWidth = 0, msaaHeight = 0;
    WGPUTextureFormat msaaFormat = WGPUTextureFormat_Undefined;
    std::unordered_map<uint64_t, WGPURenderPipeline> pipelines;
    std::unordered_map<uint32_t, WGPUSampler> samplers;
    std::unordered_map<Gfx::Texture, TextureSlot> textures;
    TextureSlot white;
    Gfx::Texture nextTexture = 1;
    // Cuadro en grabación.
    std::vector<Vertex> vertices;
    std::vector<uint8_t> uniforms;
    std::vector<Pass> passes;
    std::vector<uint8_t> skins;
    std::vector<float> palette;
    uint64_t paletteRevision = ~0ull;
    int32_t paletteBase = 0;
    uint64_t matrixRevision = ~0ull, materialRevision = ~0ull;
    uint32_t lastUniform = 0;
    int lastReceiver = 0;   // bit 0: recibe sombra, bit 1: recibe luz
    uint64_t frame = 1;
    // Recursos que siguen en uso por el cuadro grabado y se liberan al enviarlo.
    std::vector<WGPUTexture> retiredTextures;
    std::vector<WGPUTextureView> retiredViews;
    std::vector<WGPUBindGroup> retiredGroups;
    std::vector<WGPUBuffer> retiredBuffers;
    std::vector<std::unique_ptr<Staging>> staging;
    float clearColor[4]{};
    int unpackAlignment = 4;
    // Nitidez (CAS): copia del cuadro y su pipeline.
    WGPUShaderModule postModule = nullptr;
    WGPUBindGroupLayout postLayout = nullptr;
    WGPUPipelineLayout postPipelineLayout = nullptr;
    WGPURenderPipeline postPipeline = nullptr, fxaaPipeline = nullptr;
    WGPUSampler postSampler = nullptr;
    WGPUTexture postTexture = nullptr;
    WGPUTextureView postView = nullptr;
    WGPUBuffer postUniform = nullptr;
    WGPURenderPipeline underwaterPipeline = nullptr;
    // Bloom: dos texturas a un cuarto de la resolución.
    WGPUShaderModule bloomModule = nullptr;
    WGPUPipelineLayout bloomPipelineLayout = nullptr;
    WGPURenderPipeline bloomExtract = nullptr, bloomBlurH = nullptr, bloomBlurV = nullptr, bloomComposite = nullptr;
    WGPUBuffer bloomUniform = nullptr;
    WGPUTexture bloomTexture[2] = {};
    WGPUTextureView bloomView[2] = {};
    WGPUBindGroup bloomGroup[3] = {};
    WGPUTextureView bloomSource = nullptr;
    unsigned bloomWidth = 0, bloomHeight = 0;
    WGPUTextureFormat bloomFormat = WGPUTextureFormat_Undefined;
    uint32_t bloomSamples = 0;
    WGPUBindGroup postGroup = nullptr;
    unsigned postWidth = 0, postHeight = 0;
    WGPUTextureFormat postFormat = WGPUTextureFormat_Undefined;
    unsigned errors = 0;
    // Sombras proyectadas: escena en grabación y recursos.
    bool shadowScene = false, shadowFrame = false, shadowCasting = true;
    bool mapObjects = false;   // ver Gfx::SetMapObjects
    float shadowStrength = 0.5f;
    ShadowUniform shadow{};
    WGPUTexture shadowMap = nullptr;
    WGPUTextureView shadowView = nullptr;
    WGPUSampler shadowSampler = nullptr;
    WGPUBuffer shadowBuffer = nullptr;
    WGPUBindGroupLayout shadowLayout = nullptr, casterLayout = nullptr;
    WGPUBindGroup shadowGroup = nullptr, casterGroup = nullptr;
    WGPUPipelineLayout casterPipelineLayout = nullptr;
    std::unordered_map<uint8_t, WGPURenderPipeline> casterPipelines;
    // Niebla por profundidad: uniform del cuadro y recursos.
    struct Atmosphere {
        float inverseViewProj[16];
        float camera[4];     // posición de la cámara en el mundo
        float color[4];
        float params[4];     // modo, densidad, distancia inicial
        float viewport[4];   // x, y (desde arriba), ancho, alto en píxeles
        // Cielo (Gfx::Sky): horizonte + activo, cénit + nubes, dirección del
        // sol + lluvia, color del sol + tiempo.
        float sky0[4], sky1[4], sun[4], sunColor[4];
    } atmosphere{};
    float postFx[12]{};      // efectos de cámara del próximo PostProcess
    int atmosphereViewport[4]{};
    WGPUShaderModule atmosphereModule = nullptr;
    WGPUBindGroupLayout atmosphereLayout = nullptr;
    WGPUPipelineLayout atmospherePipelineLayout = nullptr;
    WGPURenderPipeline atmosphereMultiply = nullptr, atmosphereAdd = nullptr, atmosphereSky = nullptr;
    WGPUBuffer atmosphereBuffer = nullptr;
    WGPUTextureFormat atmosphereFormat = WGPUTextureFormat_Undefined;
    uint32_t atmosphereSamples = 0;
    // Mar (prueba): plano con olas reconstruido por píxel desde la profundidad.
    struct Water {
        float inverseViewProj[16];
        float viewProj[16];  // para proyectar los rayos del reflejo a pantalla
        float camera[4];
        float params[4];     // altura del plano, tiempo en segundos, y desde la que empieza el fondo
        float sun[4];        // posición del sol en el mundo
        float viewport[4];
        float lightCount[4]; // [0] = cantidad de faroles
        float lights[32][4]; // por luz (hasta 16): posición, color
        float ships[4][4];   // por barco: proa (x, y), rumbo (x, y)
        // Agua de los mapas (params.w = 1): color del cuerpo + cuánto se ve la
        // textura original, color del cielo + fuerza de las olas.
        float tint[4];
        float sky[4];
        float sim[4];        // ondas: origen (x, y), tamaño del texel, activa
    } water{};
    // Ondas del agua de los mapas: altura y velocidad en una grilla de
    // SimSize x SimSize alrededor del héroe, simuladas con la ecuación de onda.
    WGPUTexture simTexture[2] = {};
    WGPUTextureView simView[2] = {};
    int simCurrent = 0;
    WGPUShaderModule simModule = nullptr;
    WGPUBindGroupLayout simLayout = nullptr;
    WGPUPipelineLayout simPipelineLayout = nullptr;
    WGPUComputePipeline simPipeline = nullptr;
    WGPUBuffer simBuffer[4] = {};
    float simOrigin[2] = {1.0e9f, 1.0e9f};
    float simTime = -1.0f;
    float simFocus[2] = {};
    float simSources[8][4] = {};
    int simSourceCount = 0;
    float simApplied[8][4] = {};   // lo que ya está desplazando agua
    int simAppliedCount = 0;
    // Máscara de agua del mapa: 256x256, un texel por vértice del terreno.
    WGPUTexture waterMask = nullptr;
    WGPUTextureView waterMaskView = nullptr;
    int waterViewport[4]{};
    WGPUShaderModule waterModule = nullptr;
    WGPUBindGroupLayout waterLayout = nullptr;
    WGPUPipelineLayout waterPipelineLayout = nullptr;
    WGPURenderPipeline waterPipeline = nullptr;
    WGPUBuffer waterBuffer = nullptr;
    // Copia del cuadro antes del agua: de ahí salen los reflejos.
    WGPUTexture waterScene = nullptr;
    WGPUTextureView waterSceneView = nullptr;
    WGPUSampler waterSampler = nullptr;
    unsigned waterSceneWidth = 0, waterSceneHeight = 0;
    WGPUTextureFormat waterSceneFormat = WGPUTextureFormat_Undefined;
    WGPUTextureFormat waterFormat = WGPUTextureFormat_Undefined;
    uint32_t waterSamples = 0;
} g;

uint32_t Samples() { return Gfx::Multisample() > 1 ? 4u : 1u; }   // WebGPU garantiza 1 y 4

WGPUStringView View(const char* text) { return WGPUStringView{text, WGPU_STRLEN}; }

void Log(const char* text) { DbgLogPublic(text); }

WGPUBlendFactor Blend(Gfx::Factor f) {
    switch (f) {
    case Gfx::Factor::Zero: return WGPUBlendFactor_Zero;
    case Gfx::Factor::One: return WGPUBlendFactor_One;
    case Gfx::Factor::SrcColor: return WGPUBlendFactor_Src;
    case Gfx::Factor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
    case Gfx::Factor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
    case Gfx::Factor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
    case Gfx::Factor::DstAlpha: return WGPUBlendFactor_DstAlpha;
    case Gfx::Factor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
    case Gfx::Factor::DstColor: return WGPUBlendFactor_Dst;
    case Gfx::Factor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
    case Gfx::Factor::SrcAlphaSaturate: return WGPUBlendFactor_SrcAlphaSaturated;
    }
    return WGPUBlendFactor_One;
}
WGPUCompareFunction Compare(Gfx::Compare c) {
    switch (c) {
    case Gfx::Compare::Never: return WGPUCompareFunction_Never;
    case Gfx::Compare::Less: return WGPUCompareFunction_Less;
    case Gfx::Compare::Equal: return WGPUCompareFunction_Equal;
    case Gfx::Compare::LessEqual: return WGPUCompareFunction_LessEqual;
    case Gfx::Compare::Greater: return WGPUCompareFunction_Greater;
    case Gfx::Compare::NotEqual: return WGPUCompareFunction_NotEqual;
    case Gfx::Compare::GreaterEqual: return WGPUCompareFunction_GreaterEqual;
    case Gfx::Compare::Always: return WGPUCompareFunction_Always;
    }
    return WGPUCompareFunction_Always;
}
int AlphaCode(Gfx::Compare c) {
    switch (c) {
    case Gfx::Compare::Never: return 1; case Gfx::Compare::Less: return 2; case Gfx::Compare::Equal: return 3;
    case Gfx::Compare::LessEqual: return 4; case Gfx::Compare::Greater: return 5; case Gfx::Compare::NotEqual: return 6;
    case Gfx::Compare::GreaterEqual: return 7; case Gfx::Compare::Always: return 0;
    }
    return 0;
}
int EnvCode(Gfx::TexEnv e) {
    switch (e) { case Gfx::TexEnv::Replace: return 1; case Gfx::TexEnv::Decal: return 2; case Gfx::TexEnv::Add: return 4; default: return 0; }
}
bool Linear(Gfx::TexValue v) {
    return v == Gfx::TexValue::Linear || v == Gfx::TexValue::LinearMipmapNearest || v == Gfx::TexValue::LinearMipmapLinear;
}
WGPUAddressMode Address(Gfx::TexValue v) { return v == Gfx::TexValue::Repeat ? WGPUAddressMode_Repeat : WGPUAddressMode_ClampToEdge; }

WGPUBuffer CreateBuffer(uint64_t size, WGPUBufferUsage usage) {
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = size;
    d.usage = usage | WGPUBufferUsage_CopyDst;
    return wgpuDeviceCreateBuffer(GetDevice(), &d);
}

WGPUSampler Sampler(const Sampling& s) {
    auto it = g.samplers.find(s.Key());
    if (it != g.samplers.end()) return it->second;
    WGPUSamplerDescriptor d = WGPU_SAMPLER_DESCRIPTOR_INIT;
    d.addressModeU = Address(s.wrapS);
    d.addressModeV = Address(s.wrapT);
    d.magFilter = Linear(s.magFilter) ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.minFilter = Linear(s.minFilter) ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    if (Gfx::TextureMipmaps()) {
        // DESVIACION: con la opción de calidad, reducción trilineal con
        // anisotropía, también para la nítida (la UI se dibuja a su tamaño).
        d.minFilter = WGPUFilterMode_Linear;
        d.mipmapFilter = WGPUMipmapFilterMode_Linear;
        if (Linear(s.magFilter)) d.maxAnisotropy = 16;
    } else {
        // El cliente sube sólo el nivel 0: un filtro nítido no usa mipmaps.
        d.lodMaxClamp = 0.0f;
    }
    WGPUSampler sampler = wgpuDeviceCreateSampler(GetDevice(), &d);
    g.samplers.emplace(s.Key(), sampler);
    return sampler;
}

void Retire(TextureSlot& slot) {
    for (auto& entry : slot.groups) g.retiredGroups.push_back(entry.group);
    slot.groups.clear();
    if (slot.view) g.retiredViews.push_back(slot.view);
    if (slot.texture) g.retiredTextures.push_back(slot.texture);
    slot.view = nullptr; slot.texture = nullptr;
}
void Destroy(TextureSlot& slot) {
    for (auto& entry : slot.groups) wgpuBindGroupRelease(entry.group);
    slot.groups.clear();
    if (slot.view) wgpuTextureViewRelease(slot.view);
    if (slot.texture) wgpuTextureRelease(slot.texture);
    slot.view = nullptr; slot.texture = nullptr;
}

uint32_t MipLevels(int width, int height) {
    if (!Gfx::TextureMipmaps()) return 1;
    uint32_t levels = 1;
    for (int size = std::max(width, height); size > 1; size /= 2) ++levels;
    return levels;
}
WGPUTexture NewTexture(int width, int height) {
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size = {uint32_t(width), uint32_t(height), 1};
    d.mipLevelCount = MipLevels(width, height);
    d.format = WGPUTextureFormat_RGBA8Unorm;
    d.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc;
    return wgpuDeviceCreateTexture(GetDevice(), &d);
}

// Píxeles del cliente (RGB o RGBA, filas alineadas como pide el unpack) a RGBA.
std::vector<uint8_t> ToRGBA(int width, int height, Gfx::Format format, const void* pixels) {
    std::vector<uint8_t> out(size_t(width) * height * 4, 255);
    if (!pixels) return out;
    const int channels = format == Gfx::Format::RGB ? 3 : 4;
    const size_t align = size_t(std::max(1, g.unpackAlignment));
    const size_t stride = (size_t(width) * channels + align - 1) / align * align;
    const auto* src = static_cast<const uint8_t*>(pixels);
    for (int y = 0; y < height; ++y) {
        const uint8_t* row = src + y * stride;
        uint8_t* dst = &out[size_t(y) * width * 4];
        if (channels == 4) std::memcpy(dst, row, size_t(width) * 4);
        else for (int x = 0; x < width; ++x) { dst[x * 4] = row[x * 3]; dst[x * 4 + 1] = row[x * 3 + 1]; dst[x * 4 + 2] = row[x * 3 + 2]; }
    }
    return out;
}

void Write(WGPUTexture texture, int x, int y, int width, int height, const std::vector<uint8_t>& rgba, uint32_t level = 0) {
    WGPUTexelCopyTextureInfo dst = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    dst.texture = texture;
    dst.mipLevel = level;
    dst.origin = {uint32_t(x), uint32_t(y), 0};
    WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
    layout.bytesPerRow = uint32_t(width) * 4;
    layout.rowsPerImage = uint32_t(height);
    const WGPUExtent3D size{uint32_t(width), uint32_t(height), 1};
    wgpuQueueWriteTexture(GetQueue(), &dst, rgba.data(), rgba.size(), &layout, &size);
}

// Niveles de mipmap de una imagen RGBA completa (promedio de 2x2), como
// glGenerateMipmap. WebGPU no los genera solo.
void WriteMipmaps(WGPUTexture texture, int width, int height, std::vector<uint8_t> rgba) {
    const uint32_t levels = MipLevels(width, height);
    for (uint32_t level = 1; level < levels; ++level) {
        const int w = std::max(1, width / 2), h = std::max(1, height / 2);
        std::vector<uint8_t> next(size_t(w) * h * 4);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    const int x0 = std::min(x * 2, width - 1), x1 = std::min(x * 2 + 1, width - 1);
                    const int y0 = std::min(y * 2, height - 1), y1 = std::min(y * 2 + 1, height - 1);
                    const int sum = rgba[(size_t(y0) * width + x0) * 4 + c] + rgba[(size_t(y0) * width + x1) * 4 + c] +
                                    rgba[(size_t(y1) * width + x0) * 4 + c] + rgba[(size_t(y1) * width + x1) * 4 + c];
                    next[(size_t(y) * w + x) * 4 + c] = uint8_t((sum + 2) / 4);
                }
        Write(texture, 0, 0, w, h, next, level);
        rgba.swap(next); width = w; height = h;
    }
}

// La textura ligada (RenderState) para escribirla. Si ya la usa el cuadro en
// grabación, pasa a una textura nueva; keep copia el contenido anterior.
TextureSlot* Writable(bool keep) {
    auto it = g.textures.find(RenderState::Get().texture);
    if (it == g.textures.end()) return nullptr;
    TextureSlot& slot = it->second;
    if (slot.texture && slot.usedFrame == g.frame) {
        WGPUTexture fresh = NewTexture(slot.width, slot.height);
        if (keep) {
            WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(GetDevice(), nullptr);
            WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
            from.texture = slot.texture; to.texture = fresh;
            const WGPUExtent3D size{uint32_t(slot.width), uint32_t(slot.height), 1};
            wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &size);
            WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuQueueSubmit(GetQueue(), 1, &commands);
            wgpuCommandBufferRelease(commands);
            wgpuCommandEncoderRelease(encoder);
        }
        Retire(slot);
        slot.texture = fresh;
        slot.view = wgpuTextureCreateView(fresh, nullptr);
    }
    return &slot;
}

// Con mipmaps, lo 3D (depth test) amplía con filtro lineal aunque la textura
// sea nítida: si no, el límite entre ampliación y reducción se ve al moverse.
// La interfaz conserva el filtro original.
WGPUBindGroup TextureGroup(TextureSlot& slot) {
    Sampling sampling = slot.sampling;
    if (Gfx::TextureMipmaps() && RenderState::Get().caps[RenderState::CapIndex(Cap::DepthTest)])
        sampling.magFilter = Gfx::TexValue::Linear;
    const uint32_t key = sampling.Key();
    for (const auto& entry : slot.groups) if (entry.key == key) return entry.group;
    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = slot.view;
    entries[1].binding = 1; entries[1].sampler = Sampler(sampling);
    WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    d.layout = g.textureLayout;
    d.entryCount = 2;
    d.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(GetDevice(), &d);
    slot.groups.push_back({key, group});
    return group;
}

WGPURenderPipeline Pipeline(const PipelineKey& key) {
    auto it = g.pipelines.find(key.Value());
    if (it != g.pipelines.end()) return it->second;
    const bool resident = (key.topology >> 4) == Resident;
    static const WGPUVertexAttribute attributes[3] = {
        {nullptr, WGPUVertexFormat_Float32x3, offsetof(Vertex, position), 0},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Vertex, color), 1},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Vertex, uv), 2},
    };
    using Static = Skinning::StaticVertex;
    static const WGPUVertexAttribute residentAttributes[4] = {
        {nullptr, WGPUVertexFormat_Float32x3, offsetof(Static, position), 0},
        {nullptr, WGPUVertexFormat_Float32, offsetof(Static, bone), 1},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Static, normal), 2},
        {nullptr, WGPUVertexFormat_Float32x2, offsetof(Static, uv), 3},
    };
    WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    buffer.stepMode = WGPUVertexStepMode_Vertex;
    buffer.arrayStride = resident ? sizeof(Static) : sizeof(Vertex);
    buffer.attributeCount = resident ? 4 : 3;
    buffer.attributes = resident ? residentAttributes : attributes;

    WGPUBlendState blend{};
    blend.color = {WGPUBlendOperation_Add, Blend(Gfx::Factor(key.src)), Blend(Gfx::Factor(key.dst))};
    blend.alpha = blend.color;
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = SurfaceFormat();
    target.blend = key.blend ? &blend : nullptr;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = g.module;
    fragment.entryPoint = View("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;

    // Sin depth test, OpenGL tampoco escribe profundidad.
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = WGPUTextureFormat_Depth24Plus;
    depth.depthWriteEnabled = key.depthTest && key.depthWrite ? WGPUOptionalBool_True : WGPUOptionalBool_False;
    depth.depthCompare = key.depthTest ? Compare(Gfx::Compare(key.depthFunc)) : WGPUCompareFunction_Always;

    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = g.pipelineLayout;
    d.multisample.count = Samples();
    d.vertex.module = g.module;
    d.vertex.entryPoint = View(resident ? "vs_skin" : "vs");
    d.vertex.bufferCount = 1;
    d.vertex.buffers = &buffer;
    d.primitive.topology = WGPUPrimitiveTopology(key.topology & 0xF);
    d.primitive.frontFace = WGPUFrontFace_CCW;
    d.primitive.cullMode = key.cull ? WGPUCullMode_Back : WGPUCullMode_None;
    d.depthStencil = &depth;
    d.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
    g.pipelines.emplace(key.Value(), pipeline);
    return pipeline;
}

// Pipeline del pase de sombras: sólo profundidad desde la luz, sin culling.
WGPURenderPipeline CasterPipeline(uint8_t topology) {
    auto it = g.casterPipelines.find(topology);
    if (it != g.casterPipelines.end()) return it->second;
    const bool resident = (topology >> 4) == Resident;
    static const WGPUVertexAttribute attributes[3] = {
        {nullptr, WGPUVertexFormat_Float32x3, offsetof(Vertex, position), 0},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Vertex, color), 1},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Vertex, uv), 2},
    };
    using Static = Skinning::StaticVertex;
    static const WGPUVertexAttribute residentAttributes[4] = {
        {nullptr, WGPUVertexFormat_Float32x3, offsetof(Static, position), 0},
        {nullptr, WGPUVertexFormat_Float32, offsetof(Static, bone), 1},
        {nullptr, WGPUVertexFormat_Float32x4, offsetof(Static, normal), 2},
        {nullptr, WGPUVertexFormat_Float32x2, offsetof(Static, uv), 3},
    };
    WGPUVertexBufferLayout buffer = WGPU_VERTEX_BUFFER_LAYOUT_INIT;
    buffer.stepMode = WGPUVertexStepMode_Vertex;
    buffer.arrayStride = resident ? sizeof(Static) : sizeof(Vertex);
    buffer.attributeCount = resident ? 4 : 3;
    buffer.attributes = resident ? residentAttributes : attributes;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = g.module;
    fragment.entryPoint = View("fs_caster");
    fragment.targetCount = 0;
    WGPUDepthStencilState depth = WGPU_DEPTH_STENCIL_STATE_INIT;
    depth.format = WGPUTextureFormat_Depth32Float;
    depth.depthWriteEnabled = WGPUOptionalBool_True;
    depth.depthCompare = WGPUCompareFunction_Less;
    // Sesgo por pendiente: evita que las superficies se sombreen a sí mismas.
    depth.depthBias = 4;
    depth.depthBiasSlopeScale = 2.0f;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = g.casterPipelineLayout;
    d.vertex.module = g.module;
    d.vertex.entryPoint = View(resident ? "vs_skin_caster" : "vs_caster");
    d.vertex.bufferCount = 1;
    d.vertex.buffers = &buffer;
    d.primitive.topology = WGPUPrimitiveTopology(topology & 0xF);
    d.primitive.cullMode = WGPUCullMode_None;
    d.depthStencil = &depth;
    d.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
    g.casterPipelines.emplace(topology, pipeline);
    return pipeline;
}

// Uniforms del lote: se reutiliza el tramo anterior si no cambió nada.
uint32_t UniformOffset() {
    const uint64_t matrices = RenderState::MatrixRevision(), material = RenderState::MaterialRevision();
    const auto& state = RenderState::Get();
    const bool additive = state.caps[RenderState::CapIndex(Cap::Blend)] && state.blendDestination == Gfx::Factor::One;
    const bool receiver = g.shadowScene && g.shadowFrame && state.caps[RenderState::CapIndex(Cap::DepthTest)] && !additive;
    // Recibe luz: la geometría del mundo (lo que escribe profundidad y no es
    // aditivo). Los efectos semitransparentes no: son quads que ya brillan
    // solos, y con luz encima se les ve el borde del quad.
    const bool lit = g.shadowScene && state.caps[RenderState::CapIndex(Cap::DepthTest)] && state.depthWrite && !additive;
    const int receives = (receiver ? 1 : 0) | (lit ? 2 : 0);
    if (!g.uniforms.empty() && matrices == g.matrixRevision && material == g.materialRevision &&
        receives == g.lastReceiver) return g.lastUniform;
    Uniforms u{};
    std::memcpy(u.mvp, RenderState::ModelViewProjection(), sizeof(u.mvp));
    std::memcpy(u.modelView, RenderState::Top(Gfx::Matrix::ModelView), sizeof(u.modelView));
    std::memcpy(u.textureMatrix, RenderState::Top(Gfx::Matrix::Texture), sizeof(u.textureMatrix));
    const auto& s = RenderState::Get();
    u.flags[0] = s.caps[RenderState::CapIndex(Cap::Texture2D)] ? 1 : 0;
    u.flags[1] = EnvCode(s.texEnv);
    u.flags[2] = s.caps[RenderState::CapIndex(Cap::AlphaTest)] ? AlphaCode(s.alphaFunction) : 0;
    u.flags[3] = !s.caps[RenderState::CapIndex(Cap::Fog)] ? 0 : s.fogMode == Gfx::FogMode::Linear ? 1 : s.fogMode == Gfx::FogMode::Exp ? 2 : 3;
    u.params[0] = s.alphaReference; u.params[1] = s.fogStart; u.params[2] = s.fogEnd; u.params[3] = s.fogDensity;
    std::memcpy(u.fogColor, s.fogColor, sizeof(u.fogColor));
    // Bicúbico sólo en lo 3D (depth test); bits 1 y 2: repetir en S y en T.
    const bool bicubic = Gfx::TextureBicubic() && s.caps[RenderState::CapIndex(Cap::DepthTest)];
    u.extra[0] = bicubic ? 1 | (s.textureRepeat[0] ? 2 : 0) | (s.textureRepeat[1] ? 4 : 0) : 0;
    u.extra[1] = receiver ? 1 : 0;
    u.extra[2] = lit ? 1 : 0;
    const uint32_t offset = uint32_t(g.uniforms.size());
    g.uniforms.resize(g.uniforms.size() + UniformStride);
    std::memcpy(&g.uniforms[offset], &u, sizeof(u));
    g.matrixRevision = matrices; g.materialRevision = material; g.lastUniform = offset; g.lastReceiver = receives;
    return offset;
}

PipelineKey StateKey(uint8_t topology) {
    const auto& s = RenderState::Get();
    PipelineKey key;
    key.topology = topology;
    key.blend = s.caps[RenderState::CapIndex(Cap::Blend)];
    key.src = uint8_t(s.blendSource); key.dst = uint8_t(s.blendDestination);
    key.depthTest = s.caps[RenderState::CapIndex(Cap::DepthTest)];
    key.depthFunc = uint8_t(s.depthFunction);
    key.depthWrite = s.depthWrite;
    key.cull = s.caps[RenderState::CapIndex(Cap::CullFace)];
    return key;
}
// Proyecta sombra: dentro de la escena, opaco o con alpha test (escribe
// profundidad) y no aditivo; sólo triángulos.
bool Caster(const PipelineKey& key) {
    const uint8_t topology = key.topology & 0xF;
    return g.shadowScene && g.shadowFrame && g.shadowCasting && key.depthTest && key.depthWrite &&
           !(key.blend && Gfx::Factor(key.dst) == Gfx::Factor::One) &&
           (topology == WGPUPrimitiveTopology_TriangleList || topology == WGPUPrimitiveTopology_TriangleStrip);
}
// Objeto del mapa aditivo: no deja profundidad, el agua no sabe que está ahí.
bool OverWater(const PipelineKey& key) {
    return g.mapObjects && key.depthTest && !key.depthWrite && key.blend &&
           Gfx::Factor(key.dst) == Gfx::Factor::One;
}
// La textura ligada si el texturizado está activo; la blanca si no.
TextureSlot& BoundSlot() {
    const auto& s = RenderState::Get();
    if (s.caps[RenderState::CapIndex(Cap::Texture2D)]) {
        auto it = g.textures.find(s.texture);
        if (it != g.textures.end() && it->second.view) return it->second;
    }
    return g.white;
}


// Color multisample del tamaño de la superficie (sólo con MSAA).
void EnsureMsaa(unsigned width, unsigned height) {
    if (Samples() == 1) return;
    const WGPUTextureFormat format = SurfaceFormat();
    if (g.msaa && g.msaaWidth == width && g.msaaHeight == height && g.msaaFormat == format) return;
    if (g.msaaView) wgpuTextureViewRelease(g.msaaView);
    if (g.msaa) wgpuTextureRelease(g.msaa);
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size = {width, height, 1};
    d.format = format;
    d.usage = WGPUTextureUsage_RenderAttachment;
    d.sampleCount = Samples();
    g.msaa = wgpuDeviceCreateTexture(GetDevice(), &d);
    g.msaaView = wgpuTextureCreateView(g.msaa, nullptr);
    g.msaaWidth = width; g.msaaHeight = height; g.msaaFormat = format;
}
// Destino de color de una pasada: el multisample (resuelto en el cuadro) o el cuadro.
void ColorTarget(WGPURenderPassColorAttachment& color) {
    if (g.msaaView) { color.view = g.msaaView; color.resolveTarget = FrameView(); }
    else color.view = FrameView();
}

Pass& CurrentPass() {
    if (g.passes.empty()) g.passes.emplace_back();
    return g.passes.back();
}

void EnsureDepth(unsigned width, unsigned height) {
    if (g.depth && g.depthWidth == width && g.depthHeight == height) return;
    if (g.depthView) wgpuTextureViewRelease(g.depthView);
    if (g.depth) wgpuTextureRelease(g.depth);
    WGPUTextureDescriptor d = WGPU_TEXTURE_DESCRIPTOR_INIT;
    d.size = {width, height, 1};
    d.format = WGPUTextureFormat_Depth24Plus;
    // Se lee en la niebla por profundidad.
    d.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    d.sampleCount = Samples();
    g.depth = wgpuDeviceCreateTexture(GetDevice(), &d);
    g.depthView = wgpuTextureCreateView(g.depth, nullptr);
    g.depthWidth = width; g.depthHeight = height;
}

// Buffer de al menos `size` bytes; si crece se recrea (el anterior ya se envió).
void Reserve(WGPUBuffer& buffer, uint64_t& capacity, uint64_t size, WGPUBufferUsage usage) {
    if (buffer && capacity >= size) return;
    if (buffer) wgpuBufferRelease(buffer);
    capacity = std::max<uint64_t>(size, std::max<uint64_t>(capacity * 2, 1u << 20));
    buffer = CreateBuffer(capacity, usage);
    const bool grouped = &buffer == &g.uniformBuffer || &buffer == &g.skinBuffer || &buffer == &g.paletteBuffer ||
                         &buffer == &g.drawBuffer;
    if (grouped && g.uniformGroup) { wgpuBindGroupRelease(g.uniformGroup); g.uniformGroup = nullptr; }
}

void ReserveQuadIndices(uint64_t quads) {
    if (g.indexBuffer && g.indexQuads >= quads) return;
    if (g.indexBuffer) wgpuBufferRelease(g.indexBuffer);
    g.indexQuads = std::max<uint64_t>(quads, std::max<uint64_t>(g.indexQuads * 2, 4096));
    std::vector<uint32_t> indices;
    indices.reserve(g.indexQuads * 6);
    for (uint32_t q = 0; q < g.indexQuads; ++q) {
        const uint32_t v = q * 4;
        indices.insert(indices.end(), {v, v + 1, v + 2, v, v + 2, v + 3});
    }
    g.indexBuffer = CreateBuffer(indices.size() * 4, WGPUBufferUsage_Index);
    wgpuQueueWriteBuffer(GetQueue(), g.indexBuffer, 0, indices.data(), indices.size() * 4);
}

void ReleaseRetired() {
    for (auto group : g.retiredGroups) wgpuBindGroupRelease(group);
    for (auto view : g.retiredViews) wgpuTextureViewRelease(view);
    for (auto texture : g.retiredTextures) wgpuTextureRelease(texture);
    for (auto buffer : g.retiredBuffers) wgpuBufferRelease(buffer);
    g.retiredGroups.clear(); g.retiredViews.clear(); g.retiredTextures.clear(); g.retiredBuffers.clear();
}

void ResetFrame() {
    g.vertices.clear(); g.uniforms.clear(); g.passes.clear();
    g.skins.clear(); g.palette.clear(); g.paletteRevision = ~0ull;
    g.matrixRevision = g.materialRevision = ~0ull;
    g.shadowFrame = false;
    ++g.frame;
}

bool CreateObjects() {
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = View(Shader);
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    g.module = wgpuDeviceCreateShaderModule(GetDevice(), &md);

    // Grupo 0: tablas de uniforms y de skin (filas de UniformStride), paleta de
    // huesos y tabla de dibujos. Se asocia una vez por pasada; cada dibujo
    // elige sus filas con firstInstance.
    WGPUBindGroupLayoutEntry uniform[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    uniform[0].binding = 0;
    uniform[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    uniform[0].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    uniform[0].buffer.minBindingSize = UniformStride;
    uniform[1].binding = 1;
    uniform[1].visibility = WGPUShaderStage_Vertex;
    uniform[1].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    uniform[1].buffer.minBindingSize = UniformStride;
    uniform[2].binding = 2;
    uniform[2].visibility = WGPUShaderStage_Vertex;
    uniform[2].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    uniform[3].binding = 3;
    uniform[3].visibility = WGPUShaderStage_Vertex;
    uniform[3].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
    WGPUBindGroupLayoutDescriptor ud = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ud.entryCount = 4; ud.entries = uniform;
    g.uniformLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &ud);

    WGPUBindGroupLayoutEntry textureEntries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    textureEntries[0].binding = 0;
    textureEntries[0].visibility = WGPUShaderStage_Fragment;
    textureEntries[0].texture.sampleType = WGPUTextureSampleType_Float;
    textureEntries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    textureEntries[1].binding = 1;
    textureEntries[1].visibility = WGPUShaderStage_Fragment;
    textureEntries[1].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor td = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    td.entryCount = 2; td.entries = textureEntries;
    g.textureLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &td);

    // Grupo 2: sombras. El pipeline normal lee el mapa; el del pase de
    // sombras sólo el uniform (el mapa es su destino).
    WGPUBindGroupLayoutEntry shadowEntries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    shadowEntries[0].binding = 0;
    shadowEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
    shadowEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
    shadowEntries[0].buffer.minBindingSize = sizeof(ShadowUniform);
    shadowEntries[1].binding = 1;
    shadowEntries[1].visibility = WGPUShaderStage_Fragment;
    shadowEntries[1].texture.sampleType = WGPUTextureSampleType_Depth;
    shadowEntries[1].texture.viewDimension = WGPUTextureViewDimension_2D;
    shadowEntries[2].binding = 2;
    shadowEntries[2].visibility = WGPUShaderStage_Fragment;
    shadowEntries[2].sampler.type = WGPUSamplerBindingType_Comparison;
    WGPUBindGroupLayoutDescriptor sd = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    sd.entryCount = 3; sd.entries = shadowEntries;
    g.shadowLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &sd);
    sd.entryCount = 1;
    g.casterLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &sd);

    WGPUBindGroupLayout layouts[3] = {g.uniformLayout, g.textureLayout, g.shadowLayout};
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 3; pd.bindGroupLayouts = layouts;
    g.pipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
    layouts[2] = g.casterLayout;
    g.casterPipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);

    WGPUTextureDescriptor mapd = WGPU_TEXTURE_DESCRIPTOR_INIT;
    mapd.size = {ShadowSize, ShadowSize, 1};
    mapd.format = WGPUTextureFormat_Depth32Float;
    mapd.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_TextureBinding;
    g.shadowMap = wgpuDeviceCreateTexture(GetDevice(), &mapd);
    g.shadowView = wgpuTextureCreateView(g.shadowMap, nullptr);
    WGPUSamplerDescriptor smp = WGPU_SAMPLER_DESCRIPTOR_INIT;
    smp.magFilter = WGPUFilterMode_Linear;
    smp.minFilter = WGPUFilterMode_Linear;
    smp.compare = WGPUCompareFunction_LessEqual;
    g.shadowSampler = wgpuDeviceCreateSampler(GetDevice(), &smp);
    g.shadowBuffer = CreateBuffer(sizeof(ShadowUniform), WGPUBufferUsage_Uniform);
    WGPUBindGroupEntry shadowGroup[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    shadowGroup[0].binding = 0; shadowGroup[0].buffer = g.shadowBuffer; shadowGroup[0].size = sizeof(ShadowUniform);
    shadowGroup[1].binding = 1; shadowGroup[1].textureView = g.shadowView;
    shadowGroup[2].binding = 2; shadowGroup[2].sampler = g.shadowSampler;
    WGPUBindGroupDescriptor gd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    gd.layout = g.shadowLayout; gd.entryCount = 3; gd.entries = shadowGroup;
    g.shadowGroup = wgpuDeviceCreateBindGroup(GetDevice(), &gd);
    gd.layout = g.casterLayout; gd.entryCount = 1;
    g.casterGroup = wgpuDeviceCreateBindGroup(GetDevice(), &gd);

    // Textura blanca 1x1 para los lotes sin textura (el shader muestrea siempre).
    g.white.texture = NewTexture(1, 1);
    g.white.view = wgpuTextureCreateView(g.white.texture, nullptr);
    g.white.width = g.white.height = 1;
    Write(g.white.texture, 0, 0, 1, 1, std::vector<uint8_t>(4, 255));
    return g.module && g.uniformLayout && g.textureLayout && g.pipelineLayout && g.white.view &&
           g.shadowGroup && g.casterGroup && g.casterPipelineLayout;
}

void ReleaseObjects() {
    ResetFrame();
    ReleaseRetired();
    for (auto& [handle, slot] : g.textures) Destroy(slot);
    Destroy(g.white);
    for (auto& [key, pipeline] : g.pipelines) wgpuRenderPipelineRelease(pipeline);
    for (auto& [key, pipeline] : g.casterPipelines) wgpuRenderPipelineRelease(pipeline);
    if (g.atmosphereMultiply) wgpuRenderPipelineRelease(g.atmosphereMultiply);
    if (g.atmosphereAdd) wgpuRenderPipelineRelease(g.atmosphereAdd);
    if (g.atmosphereSky) wgpuRenderPipelineRelease(g.atmosphereSky);
    if (g.atmospherePipelineLayout) wgpuPipelineLayoutRelease(g.atmospherePipelineLayout);
    if (g.atmosphereLayout) wgpuBindGroupLayoutRelease(g.atmosphereLayout);
    if (g.atmosphereModule) wgpuShaderModuleRelease(g.atmosphereModule);
    if (g.atmosphereBuffer) wgpuBufferRelease(g.atmosphereBuffer);
    if (g.waterPipeline) wgpuRenderPipelineRelease(g.waterPipeline);
    if (g.waterPipelineLayout) wgpuPipelineLayoutRelease(g.waterPipelineLayout);
    if (g.waterLayout) wgpuBindGroupLayoutRelease(g.waterLayout);
    if (g.waterModule) wgpuShaderModuleRelease(g.waterModule);
    if (g.waterBuffer) wgpuBufferRelease(g.waterBuffer);
    if (g.waterSceneView) wgpuTextureViewRelease(g.waterSceneView);
    if (g.waterScene) wgpuTextureRelease(g.waterScene);
    if (g.waterSampler) wgpuSamplerRelease(g.waterSampler);
    if (g.waterMaskView) wgpuTextureViewRelease(g.waterMaskView);
    if (g.waterMask) wgpuTextureRelease(g.waterMask);
    for (int i = 0; i < 2; ++i) {
        if (g.simView[i]) wgpuTextureViewRelease(g.simView[i]);
        if (g.simTexture[i]) wgpuTextureRelease(g.simTexture[i]);
    }
    for (WGPUBuffer b : g.simBuffer) if (b) wgpuBufferRelease(b);
    if (g.simPipeline) wgpuComputePipelineRelease(g.simPipeline);
    if (g.simPipelineLayout) wgpuPipelineLayoutRelease(g.simPipelineLayout);
    if (g.simLayout) wgpuBindGroupLayoutRelease(g.simLayout);
    if (g.simModule) wgpuShaderModuleRelease(g.simModule);
    if (g.shadowGroup) wgpuBindGroupRelease(g.shadowGroup);
    if (g.casterGroup) wgpuBindGroupRelease(g.casterGroup);
    if (g.shadowBuffer) wgpuBufferRelease(g.shadowBuffer);
    if (g.shadowSampler) wgpuSamplerRelease(g.shadowSampler);
    if (g.shadowView) wgpuTextureViewRelease(g.shadowView);
    if (g.shadowMap) wgpuTextureRelease(g.shadowMap);
    if (g.casterPipelineLayout) wgpuPipelineLayoutRelease(g.casterPipelineLayout);
    if (g.casterLayout) wgpuBindGroupLayoutRelease(g.casterLayout);
    if (g.shadowLayout) wgpuBindGroupLayoutRelease(g.shadowLayout);
    for (auto& staging : g.staging) {
        wgpuBufferRelease(staging->buffer);
        if (staging->mapped) continue;
        // Con el mapeo en curso, el callback todavía apunta al objeto: queda
        // huérfano y lo borra el callback (que llega con el estado de abortado).
        staging->orphan = true;
        (void)staging.release();
    }
    for (auto& [key, sampler] : g.samplers) wgpuSamplerRelease(sampler);
    for (auto& [key, mesh] : g.meshes) {
        if (mesh.indices) wgpuBufferRelease(mesh.indices);
        if (mesh.vertices) wgpuBufferRelease(mesh.vertices);
    }
    if (g.skinBuffer) wgpuBufferRelease(g.skinBuffer);
    if (g.paletteBuffer) wgpuBufferRelease(g.paletteBuffer);
    if (g.drawBuffer) wgpuBufferRelease(g.drawBuffer);
    if (g.uniformGroup) wgpuBindGroupRelease(g.uniformGroup);
    if (g.vertexBuffer) wgpuBufferRelease(g.vertexBuffer);
    if (g.uniformBuffer) wgpuBufferRelease(g.uniformBuffer);
    if (g.indexBuffer) wgpuBufferRelease(g.indexBuffer);
    if (g.depthView) wgpuTextureViewRelease(g.depthView);
    if (g.msaaView) wgpuTextureViewRelease(g.msaaView);
    if (g.msaa) wgpuTextureRelease(g.msaa);
    if (g.postGroup) wgpuBindGroupRelease(g.postGroup);
    if (g.postView) wgpuTextureViewRelease(g.postView);
    if (g.postTexture) wgpuTextureRelease(g.postTexture);
    if (g.postUniform) wgpuBufferRelease(g.postUniform);
    if (g.postPipeline) wgpuRenderPipelineRelease(g.postPipeline);
    if (g.fxaaPipeline) wgpuRenderPipelineRelease(g.fxaaPipeline);
    if (g.postSampler) wgpuSamplerRelease(g.postSampler);
    if (g.postPipelineLayout) wgpuPipelineLayoutRelease(g.postPipelineLayout);
    if (g.postLayout) wgpuBindGroupLayoutRelease(g.postLayout);
    if (g.postModule) wgpuShaderModuleRelease(g.postModule);
    if (g.depth) wgpuTextureRelease(g.depth);
    if (g.pipelineLayout) wgpuPipelineLayoutRelease(g.pipelineLayout);
    if (g.textureLayout) wgpuBindGroupLayoutRelease(g.textureLayout);
    if (g.uniformLayout) wgpuBindGroupLayoutRelease(g.uniformLayout);
    if (g.module) wgpuShaderModuleRelease(g.module);
    g = State{};
}
WGPUBuffer UploadBuffer(const void* data, uint64_t size, WGPUBufferUsage usage) {
    WGPUBuffer buffer = CreateBuffer((size + 3) & ~uint64_t(3), usage);
    wgpuQueueWriteBuffer(GetQueue(), buffer, 0, data, size);
    return buffer;
}
void RetireMesh(GpuMesh& mesh, bool vertices) {
    WGPUBuffer& buffer = vertices ? mesh.vertices : mesh.indices;
    if (!buffer) return;
    if (mesh.usedFrame == g.frame) g.retiredBuffers.push_back(buffer); else wgpuBufferRelease(buffer);
    buffer = nullptr;
}
} // namespace

// Graba una malla residente con la pose capturada. false si la pose no
// corresponde al dibujo: el caller dibuja por el camino de referencia.
bool DrawResidentMesh(const void* key, MeshTopology::Mesh& topology, const Skinning::ResidentDraw& d) {
    if (!g.ready) return false;
    const Skinning::Pose::Mesh* pose = Skinning::Pose::Find(d.model, d.key, d.world, d.positionCount);
    if (!pose) return false;
    const auto& bones = Skinning::Pose::Palette();
    if (d.maxBone < 0 || std::size_t(d.maxBone) * 12 >= bones.size()) return false;
    if (d.lit && !Skinning::Pose::LightMatches(*pose, d.colors, d.normalCount, d.bodyLight)) return false;
    BufferedDraw::Flush();

    GpuMesh& mesh = g.meshes[key];
    if (!mesh.indices || mesh.revision != topology.revision) {
        RetireMesh(mesh, false); RetireMesh(mesh, true);
        mesh.indices = UploadBuffer(topology.indices.data(), topology.indices.size() * sizeof(unsigned), WGPUBufferUsage_Index);
        mesh.revision = topology.revision; mesh.residentRevision = 0;
    }
    if (!mesh.vertices || mesh.residentRevision != topology.residentRevision) {
        RetireMesh(mesh, true);
        mesh.vertices = UploadBuffer(topology.resident.data(), topology.resident.size() * sizeof(Skinning::StaticVertex), WGPUBufferUsage_Vertex);
        mesh.residentRevision = topology.residentRevision;
    }
    mesh.usedFrame = g.frame;

    // La paleta de la pose se agrega una vez por cuadro y por pose.
    if (g.paletteRevision != Skinning::Pose::Revision()) {
        g.paletteBase = int32_t(g.palette.size() / 4);
        g.palette.insert(g.palette.end(), bones.begin(), bones.end());
        g.paletteRevision = Skinning::Pose::Revision();
    }
    Skin skin{};
    skin.paletteBase = g.paletteBase;
    skin.flags = (d.lit ? 1u : 0u) | (d.shadow ? 2u : 0u) | (d.textured ? 4u : 0u) | (d.chromeMap ? 8u : 0u);
    if (d.chromeMap) { std::memcpy(skin.chromeU, d.chromeMap, 4 * sizeof(float)); std::memcpy(skin.chromeV, d.chromeMap + 4, 4 * sizeof(float)); }
    skin.scales[0] = Skinning::Pose::RotationScale(); skin.scales[1] = Skinning::Pose::BodyScale();
    std::memcpy(skin.origin, Skinning::Pose::Origin(), 3 * sizeof(float));
    std::memcpy(skin.direction, Skinning::Pose::Direction(), 3 * sizeof(float));
    std::memcpy(skin.coefficients, Skinning::Pose::Coefficients(), 3 * sizeof(float));
    if (d.bodyLight) std::memcpy(skin.bodyLight, d.bodyLight, 3 * sizeof(float));
    skin.uvOffset[0] = d.uOffset; skin.uvOffset[1] = d.vOffset;
    if (d.shadow && d.shadowOrigin) std::memcpy(skin.shadowOrigin, d.shadowOrigin, 3 * sizeof(float));
    if (d.color) std::memcpy(skin.color, d.color, 4 * sizeof(float));
    if (d.uv) std::memcpy(skin.uv, d.uv, 4 * sizeof(float));
    const uint32_t skinOffset = uint32_t(g.skins.size());
    g.skins.resize(g.skins.size() + UniformStride);
    std::memcpy(&g.skins[skinOffset], &skin, sizeof(skin));

    TextureSlot& slot = BoundSlot();
    Draw draw{};
    const PipelineKey residentKey = StateKey(uint8_t(WGPUPrimitiveTopology_TriangleList | (Resident << 4)));
    draw.pipeline = Pipeline(residentKey);
    draw.caster = Caster(residentKey) && !d.shadow;   // la sombra plana no proyecta
    draw.overWater = OverWater(residentKey);
    draw.textureGroup = TextureGroup(slot);
    slot.usedFrame = g.frame;
    draw.uniformOffset = UniformOffset();
    draw.skinOffset = skinOffset;
    std::memcpy(draw.viewport, RenderState::Get().viewport, sizeof(draw.viewport));
    draw.vertexBuffer = mesh.vertices;
    draw.indexBuffer = mesh.indices;
    draw.indexCount = uint32_t(d.indices);
    CurrentPass().draws.push_back(draw);
    return true;
}

void SetResident(bool enabled) { g.residentEnabled = enabled; }
bool ResidentEnabled() { return g.residentEnabled; }
void ReleaseMeshes() {
    for (auto& [key, mesh] : g.meshes) { RetireMesh(mesh, false); RetireMesh(mesh, true); }
    g.meshes.clear();
}

namespace {
const char* const PostShader = R"WGSL(
// amount = nitidez. Efectos de cámara: p0 = bajo el agua, tiempo, ondulación,
// aberración; p1 = tinte + viñeteado; p2 = rayos; viewport = la imagen 3D.
struct Post { amount: f32, pad0: f32, pad1: f32, pad2: f32, p0: vec4f, p1: vec4f, p2: vec4f, viewport: vec4f };
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> post: Post;
@group(0) @binding(2) var smp: sampler;
@vertex fn vs_post(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(select(-1.0, 3.0, i == 1u), select(-1.0, 3.0, i == 2u));
    return vec4f(p, 0.0, 1.0);
}
fn at(p: vec2i) -> vec3f {
    let size = vec2i(textureDimensions(src, 0));
    return textureLoad(src, clamp(p, vec2i(0), size - 1), 0).rgb;
}
@fragment fn fs_post(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let p = vec2i(position.xy);
    let a = at(p + vec2i(-1, -1)); let b = at(p + vec2i(0, -1)); let c = at(p + vec2i(1, -1));
    let d = at(p + vec2i(-1, 0));  let e = at(p);                let f = at(p + vec2i(1, 0));
    let g = at(p + vec2i(-1, 1));  let h = at(p + vec2i(0, 1));  let i = at(p + vec2i(1, 1));
    var mn = min(min(min(d, e), min(f, b)), h); mn = mn + min(mn, min(min(a, c), min(g, i)));
    var mx = max(max(max(d, e), max(f, b)), h); mx = mx + max(mx, max(max(a, c), max(g, i)));
    let amp = sqrt(clamp(min(mn, 2.0 - mx) / max(mx, vec3f(1e-5)), vec3f(0.0), vec3f(1.0)));
    // Realce del laplaciano, atenuado por amp en las zonas de mucho contraste.
    // Las diferencias muy chicas (ruido, transición entre mipmaps) se ignoran.
    var l = 4.0 * e - b - d - f - h;
    l = sign(l) * max(abs(l) - vec3f(0.06), vec3f(0.0));
    return vec4f(clamp(e + post.amount * amp * l, vec3f(0.0), vec3f(1.0)), 1.0);
}
fn lum(c: vec3f) -> f32 { return dot(c, vec3f(0.299, 0.587, 0.114)); }
fn sampleAt(uv: vec2f) -> vec3f { return textureSampleLevel(src, smp, uv, 0.0).rgb; }
// DESVIACION: bajo el agua (Atlans). La imagen ondula como vista a través del
// agua, los bordes se separan en colores, el color va hacia el azul verdoso,
// bajan rayos de luz desde arriba y se oscurecen los bordes.
@fragment fn fs_underwater(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let texel = 1.0 / vec2f(textureDimensions(src, 0));
    let local = (position.xy - post.viewport.xy) / max(post.viewport.zw, vec2f(1.0));
    let t = post.p0.y;
    let wobble = vec2f(sin(local.y * 31.0 + t * 1.9) + 0.5 * sin(local.y * 13.0 - t * 1.3),
                       cos(local.x * 27.0 + t * 1.6) + 0.5 * cos(local.x * 11.0 + t * 1.1)) * post.p0.z * texel;
    let uv = position.xy * texel + wobble;
    let dir = local - vec2f(0.5);
    let ab = dir * post.p0.w * 6.0 * texel;
    var c = vec3f(sampleAt(uv + ab).r, sampleAt(uv).g, sampleAt(uv - ab).b);
    c = c * post.p1.rgb;
    let a = local.x * 0.9 + local.y * 0.45;
    var ray = pow(sin(a * 23.0 + t * 0.35) * 0.5 + 0.5, 6.0) * 0.6 + pow(sin(a * 9.0 - t * 0.22) * 0.5 + 0.5, 4.0) * 0.4;
    ray = ray * pow(clamp(1.0 - local.y, 0.0, 1.0), 1.5);
    c = c + vec3f(0.35, 0.62, 0.82) * ray * post.p2.x * 0.35;
    c = c * mix(1.0, smoothstep(1.0, 0.25, length(dir * vec2f(1.0, 1.25))), post.p1.w);
    return vec4f(clamp(c, vec3f(0.0), vec3f(1.0)), 1.0);
}
// FXAA (versión de consola de Timothy Lottes).
@fragment fn fs_fxaa(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let texel = 1.0 / vec2f(textureDimensions(src, 0));
    let uv = position.xy * texel;
    let rgbM = sampleAt(uv);
    let nw = lum(sampleAt(uv + vec2f(-1.0, -1.0) * texel)); let ne = lum(sampleAt(uv + vec2f(1.0, -1.0) * texel));
    let sw = lum(sampleAt(uv + vec2f(-1.0, 1.0) * texel));  let se = lum(sampleAt(uv + vec2f(1.0, 1.0) * texel));
    let m = lum(rgbM);
    let lmin = min(m, min(min(nw, ne), min(sw, se)));
    let lmax = max(m, max(max(nw, ne), max(sw, se)));
    // Sólo bordes con contraste real: las texturas quedan intactas.
    if (lmax - lmin < max(0.0625, lmax * 0.125)) { return vec4f(rgbM, 1.0); }
    var dir = vec2f(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * 0.03125, 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), vec2f(-8.0), vec2f(8.0)) * texel;
    let a = 0.5 * (sampleAt(uv + dir * (1.0 / 3.0 - 0.5)) + sampleAt(uv + dir * (2.0 / 3.0 - 0.5)));
    let b = a * 0.5 + 0.25 * (sampleAt(uv - dir * 0.5) + sampleAt(uv + dir * 0.5));
    let lb = lum(b);
    return vec4f(select(b, a, lb < lmin || lb > lmax), 1.0);
}
)WGSL";

WGPURenderPipeline PostPipeline(const char* entry, WGPUTextureFormat format) {
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = g.postModule;
    fragment.entryPoint = View(entry);
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = g.postPipelineLayout;
    d.multisample.count = Samples();
    d.vertex.module = g.postModule;
    d.vertex.entryPoint = View("vs_post");
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.fragment = &fragment;
    return wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
}

// Objetos del posproceso; los pipelines dependen del formato de la superficie.
bool EnsurePost(unsigned width, unsigned height) {
    const WGPUTextureFormat format = SurfaceFormat();
    if (!g.postModule) {
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = View(PostShader);
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &wgsl.chain;
        g.postModule = wgpuDeviceCreateShaderModule(GetDevice(), &md);
        WGPUBindGroupLayoutEntry entries[3] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].buffer.type = WGPUBufferBindingType_Uniform;
        entries[1].buffer.minBindingSize = PostUniformSize;
        entries[2].binding = 2;
        entries[2].visibility = WGPUShaderStage_Fragment;
        entries[2].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
        ld.entryCount = 3; ld.entries = entries;
        g.postLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &ld);
        WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pd.bindGroupLayoutCount = 1; pd.bindGroupLayouts = &g.postLayout;
        g.postPipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
        g.postUniform = CreateBuffer(PostUniformSize, WGPUBufferUsage_Uniform);
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        g.postSampler = wgpuDeviceCreateSampler(GetDevice(), &sd);
    }
    if (!g.postModule || !g.postLayout || !g.postPipelineLayout || !g.postUniform || !g.postSampler) return false;
    if (!g.postPipeline || g.postFormat != format) {
        if (g.postPipeline) wgpuRenderPipelineRelease(g.postPipeline);
        if (g.fxaaPipeline) wgpuRenderPipelineRelease(g.fxaaPipeline);
        if (g.underwaterPipeline) wgpuRenderPipelineRelease(g.underwaterPipeline);
        g.postPipeline = PostPipeline("fs_post", format);
        g.fxaaPipeline = PostPipeline("fs_fxaa", format);
        g.underwaterPipeline = PostPipeline("fs_underwater", format);
        g.postFormat = format;
        if (g.postGroup) { wgpuBindGroupRelease(g.postGroup); g.postGroup = nullptr; }
        if (g.postView) { wgpuTextureViewRelease(g.postView); g.postView = nullptr; }
        if (g.postTexture) { wgpuTextureRelease(g.postTexture); g.postTexture = nullptr; }
    }
    if (!g.postTexture || g.postWidth != width || g.postHeight != height) {
        if (g.postGroup) { wgpuBindGroupRelease(g.postGroup); g.postGroup = nullptr; }
        if (g.postView) wgpuTextureViewRelease(g.postView);
        if (g.postTexture) wgpuTextureRelease(g.postTexture);
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = {width, height, 1};
        td.format = format;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        g.postTexture = wgpuDeviceCreateTexture(GetDevice(), &td);
        g.postView = wgpuTextureCreateView(g.postTexture, nullptr);
        g.postWidth = width; g.postHeight = height;
    }
    if (!g.postGroup) {
        WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        entries[0].binding = 0; entries[0].textureView = g.postView;
        entries[1].binding = 1; entries[1].buffer = g.postUniform; entries[1].size = PostUniformSize;
        entries[2].binding = 2; entries[2].sampler = g.postSampler;
        WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        d.layout = g.postLayout; d.entryCount = 3; d.entries = entries;
        g.postGroup = wgpuDeviceCreateBindGroup(GetDevice(), &d);
    }
    return g.postPipeline && g.fxaaPipeline && g.underwaterPipeline && g.postGroup;
}

// ── Bloom ───────────────────────────────────────────────────────────────────
// DESVIACION: lo que pasa de cierto brillo se extrae a un cuarto de la
// resolución, se desenfoca en dos pasadas y se suma a la imagen.
const char* const BloomShader = R"WGSL(
struct Bloom { params: vec4f, sizes: vec4f };   // umbral, intensidad; cuarto (ancho, alto), cuadro (ancho, alto)
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> b: Bloom;
@group(0) @binding(2) var smp: sampler;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(select(-1.0, 3.0, i == 1u), select(-1.0, 3.0, i == 2u));
    return vec4f(p, 0.0, 1.0);
}
fn at(uv: vec2f) -> vec3f { return textureSampleLevel(src, smp, uv, 0.0).rgb; }
@fragment fn fs_extract(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let uv = position.xy / b.sizes.xy;
    let o = 1.0 / b.sizes.zw;
    let c = (at(uv + vec2f(-o.x, -o.y)) + at(uv + vec2f(o.x, -o.y)) + at(uv + vec2f(-o.x, o.y)) + at(uv + vec2f(o.x, o.y))) * 0.25;
    let l = max(max(c.r, c.g), c.b);
    let k = max(l - b.params.x, 0.0) / max(1.0 - b.params.x, 1e-3);
    return vec4f(c * clamp(k, 0.0, 1.0), 1.0);
}
fn blur(position: vec4f, axis: vec2f) -> vec4f {
    let texel = 1.0 / vec2f(textureDimensions(src, 0));
    let uv = position.xy * texel;
    var w = array<f32, 5>(0.227, 0.194, 0.122, 0.054, 0.016);
    var c = at(uv) * w[0];
    for (var i = 1; i < 5; i++) {
        let d = axis * texel * f32(i) * 1.5;
        c += (at(uv + d) + at(uv - d)) * w[i];
    }
    return vec4f(c, 1.0);
}
@fragment fn fs_blur_h(@builtin(position) position: vec4f) -> @location(0) vec4f { return blur(position, vec2f(1.0, 0.0)); }
@fragment fn fs_blur_v(@builtin(position) position: vec4f) -> @location(0) vec4f { return blur(position, vec2f(0.0, 1.0)); }
@fragment fn fs_composite(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return vec4f(at(position.xy / b.sizes.zw) * b.params.y, 0.0);
}
)WGSL";

bool EnsureBloom(unsigned width, unsigned height) {
    const WGPUTextureFormat format = SurfaceFormat();
    const uint32_t samples = Samples();
    if (!g.bloomModule) {
        WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
        wgsl.code = View(BloomShader);
        WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
        md.nextInChain = &wgsl.chain;
        g.bloomModule = wgpuDeviceCreateShaderModule(GetDevice(), &md);
        g.bloomUniform = CreateBuffer(PostUniformSize, WGPUBufferUsage_Uniform);
        WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
        pd.bindGroupLayoutCount = 1; pd.bindGroupLayouts = &g.postLayout;
        g.bloomPipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
    }
    if (!g.bloomModule || !g.bloomUniform || !g.bloomPipelineLayout) return false;
    auto pipeline = [&](const char* entry, WGPUTextureFormat target, uint32_t count, bool additive) {
        WGPUBlendState blend{};
        blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_One, WGPUBlendFactor_One};
        blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
        WGPUColorTargetState t = WGPU_COLOR_TARGET_STATE_INIT;
        t.format = target;
        if (additive) t.blend = &blend;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = g.bloomModule;
        fragment.entryPoint = View(entry);
        fragment.targetCount = 1;
        fragment.targets = &t;
        WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        d.layout = g.bloomPipelineLayout;
        d.multisample.count = count;
        d.vertex.module = g.bloomModule;
        d.vertex.entryPoint = View("vs");
        d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        d.fragment = &fragment;
        return wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
    };
    if (!g.bloomExtract) {
        g.bloomExtract = pipeline("fs_extract", WGPUTextureFormat_RGBA8Unorm, 1, false);
        g.bloomBlurH = pipeline("fs_blur_h", WGPUTextureFormat_RGBA8Unorm, 1, false);
        g.bloomBlurV = pipeline("fs_blur_v", WGPUTextureFormat_RGBA8Unorm, 1, false);
    }
    if (!g.bloomComposite || g.bloomFormat != format || g.bloomSamples != samples) {
        if (g.bloomComposite) wgpuRenderPipelineRelease(g.bloomComposite);
        g.bloomComposite = pipeline("fs_composite", format, samples, true);
        g.bloomFormat = format; g.bloomSamples = samples;
    }
    const unsigned qw = std::max(1u, width / 4), qh = std::max(1u, height / 4);
    if (!g.bloomTexture[0] || g.bloomWidth != qw || g.bloomHeight != qh || g.bloomSource != g.postView) {
        for (int i = 0; i < 2; ++i) {
            if (g.bloomGroup[i]) { wgpuBindGroupRelease(g.bloomGroup[i]); g.bloomGroup[i] = nullptr; }
            if (g.bloomView[i]) { wgpuTextureViewRelease(g.bloomView[i]); g.bloomView[i] = nullptr; }
            if (g.bloomTexture[i]) { wgpuTextureRelease(g.bloomTexture[i]); g.bloomTexture[i] = nullptr; }
            WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
            td.size = {qw, qh, 1};
            td.format = WGPUTextureFormat_RGBA8Unorm;
            td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_RenderAttachment;
            g.bloomTexture[i] = wgpuDeviceCreateTexture(GetDevice(), &td);
            g.bloomView[i] = wgpuTextureCreateView(g.bloomTexture[i], nullptr);
        }
        if (g.bloomGroup[2]) { wgpuBindGroupRelease(g.bloomGroup[2]); g.bloomGroup[2] = nullptr; }
        // [0] lee el cuarto A, [1] el B, [2] la copia del cuadro.
        WGPUTextureView sources[3] = {g.bloomView[0], g.bloomView[1], g.postView};
        for (int i = 0; i < 3; ++i) {
            WGPUBindGroupEntry entries[3] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
            entries[0].binding = 0; entries[0].textureView = sources[i];
            entries[1].binding = 1; entries[1].buffer = g.bloomUniform; entries[1].size = PostUniformSize;
            entries[2].binding = 2; entries[2].sampler = g.postSampler;
            WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            d.layout = g.postLayout; d.entryCount = 3; d.entries = entries;
            g.bloomGroup[i] = wgpuDeviceCreateBindGroup(GetDevice(), &d);
        }
        g.bloomWidth = qw; g.bloomHeight = qh; g.bloomSource = g.postView;
    }
    return g.bloomExtract && g.bloomBlurH && g.bloomBlurV && g.bloomComposite &&
           g.bloomGroup[0] && g.bloomGroup[1] && g.bloomGroup[2];
}

// Copia el cuadro y lo redibuja con el filtro dentro del viewport de la pasada.
void EncodePost(WGPUCommandEncoder encoder, const Pass& pass, unsigned width, unsigned height) {
    const int* v = pass.postViewport;
    const float x = float(std::max(0, v[0]));
    const float w = std::min(float(v[2]), float(width) - x);
    const float top = float(int(height) - v[1] - v[3]);
    const float y = std::max(0.0f, top);
    const float h = std::min(float(v[3]) - (y - top), float(height) - y);
    if (w <= 0 || h <= 0 || !EnsurePost(width, height)) return;
    const float* fx = pass.fx;
    const float uniform[PostUniformSize / 4] = {
        pass.sharpen, 0, 0, 0,
        fx[0], fx[1], fx[2], fx[3],
        fx[4], fx[5], fx[6], fx[7],
        fx[8], 0, 0, 0,
        x, y, w, h,
    };
    wgpuQueueWriteBuffer(GetQueue(), g.postUniform, 0, uniform, sizeof(uniform));
    auto copyFrame = [&]() {
        WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = FrameTexture(); to.texture = g.postTexture;
        const WGPUExtent3D size{width, height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &size);
    };
    // Una pasada de pantalla completa sobre target (nullptr = el cuadro).
    auto fullscreen = [&](WGPURenderPipeline pipeline, WGPUBindGroup group, WGPUTextureView target,
                          float vx, float vy, float vw, float vh) {
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        if (target) { color.view = target; color.loadOp = WGPULoadOp_Clear; }
        else { ColorTarget(color); color.loadOp = WGPULoadOp_Load; }
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;
        WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetViewport(p, vx, vy, vw, vh, 0, 1);
        wgpuRenderPassEncoderSetPipeline(p, pipeline);
        wgpuRenderPassEncoderSetBindGroup(p, 0, group, 0, nullptr);
        wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(p);
        wgpuRenderPassEncoderRelease(p);
    };
    if (pass.Underwater()) { copyFrame(); fullscreen(g.underwaterPipeline, g.postGroup, nullptr, x, y, w, h); }
    if (pass.Bloom() && EnsureBloom(width, height)) {
        copyFrame();
        const float bloom[8] = {fx[10], fx[9], 0, 0, float(g.bloomWidth), float(g.bloomHeight), float(width), float(height)};
        wgpuQueueWriteBuffer(GetQueue(), g.bloomUniform, 0, bloom, sizeof(bloom));
        const float qw = float(g.bloomWidth), qh = float(g.bloomHeight);
        fullscreen(g.bloomExtract, g.bloomGroup[2], g.bloomView[0], 0, 0, qw, qh);
        fullscreen(g.bloomBlurH, g.bloomGroup[0], g.bloomView[1], 0, 0, qw, qh);
        fullscreen(g.bloomBlurV, g.bloomGroup[1], g.bloomView[0], 0, 0, qw, qh);
        fullscreen(g.bloomComposite, g.bloomGroup[0], nullptr, x, y, w, h);
    }
    auto run = [&](WGPURenderPipeline pipeline) {
        copyFrame();
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ColorTarget(color);
        color.loadOp = WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;
        WGPURenderPassEncoder p = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetViewport(p, x, y, w, h, 0, 1);
        wgpuRenderPassEncoderSetPipeline(p, pipeline);
        wgpuRenderPassEncoderSetBindGroup(p, 0, g.postGroup, 0, nullptr);
        wgpuRenderPassEncoderDraw(p, 3, 1, 0, 0);
        wgpuRenderPassEncoderEnd(p);
        wgpuRenderPassEncoderRelease(p);
    };
    if (pass.antialias) run(g.fxaaPipeline);
    if (pass.sharpen > 0.0f) run(g.postPipeline);
}

// ── Niebla por profundidad ──────────────────────────────────────────────────
// Reconstruye cada píxel desde la profundidad y mezcla con el color de la
// niebla en dos pasadas (multiplicar por la transmitancia y sumar el color),
// sin copiar la imagen.
const char* const AtmosphereShader = R"WGSL(
struct Atmosphere { inverseViewProj: mat4x4f, camera: vec4f, color: vec4f, params: vec4f, viewport: vec4f,
                    sky0: vec4f, sky1: vec4f, sun: vec4f, sunColor: vec4f };
@group(0) @binding(0) var depth: DEPTH_TYPE;
@group(0) @binding(1) var<uniform> a: Atmosphere;
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(select(-1.0, 3.0, i == 1u), select(-1.0, 3.0, i == 2u));
    return vec4f(p, 0.0, 1.0);
}
// Transmitancia por canal: 1 = sin niebla.
fn transmittance(position: vec4f) -> vec3f {
    let d = textureLoad(depth, vec2i(position.xy), 0);
    if (d >= 1.0) {
        // Fondo: todo niebla, salvo que haya cielo (la niebla ya va hacia él).
        return select(vec3f(0.0), vec3f(1.0), a.sky0.w > 0.5);
    }
    let ndc = vec2f((position.x - a.viewport.x) / a.viewport.z * 2.0 - 1.0,
                    1.0 - (position.y - a.viewport.y) / a.viewport.w * 2.0);
    var w = a.inverseViewProj * vec4f(ndc, d * 2.0 - 1.0, 1.0);
    let world = w.xyz / w.w;
    let distance = max(length(world - a.camera.xyz) - a.params.z, 0.0);
    // El borde del mapa (256 tiles de 100) se cierra con niebla: el fondo
    // tiene su color, así el final del mapa no se ve.
    // Sólo las últimas 3 casillas y lejos del personaje: en los mapas que se
    // juegan pegados al borde no tiene que verse.
    let border = min(min(world.x, world.y), min(25600.0 - world.x, 25600.0 - world.y));
    let edge = 1.0 - (1.0 - smoothstep(0.0, 300.0, border)) * smoothstep(0.0, 1000.0, distance);
    if (a.params.x > 1.5) {
        // Bajo el agua: el rojo se absorbe primero.
        return exp(-a.params.y * vec3f(3.0, 1.8, 1.2) * distance) * edge;
    }
    let x = distance * a.params.y;
    return vec3f(exp(-x * x)) * edge;
}
@fragment fn fs_multiply(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return vec4f(transmittance(position), 1.0);
}
@fragment fn fs_add(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return vec4f(a.color.rgb * (vec3f(1.0) - transmittance(position)), 0.0);
}
// DESVIACION: cielo procedural donde no se dibujó nada (profundidad 1):
// degradé del horizonte al cénit, el sol y nubes que se mueven.
fn hash2(p: vec2f) -> f32 { return fract(sin(dot(p, vec2f(127.1, 311.7))) * 43758.5453); }
fn noise2(p: vec2f) -> f32 {
    let i = floor(p); let f = fract(p);
    let w = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash2(i), hash2(i + vec2f(1.0, 0.0)), w.x), mix(hash2(i + vec2f(0.0, 1.0)), hash2(i + vec2f(1.0, 1.0)), w.x), w.y);
}
fn fbm(p0: vec2f) -> f32 {
    var p = p0; var v = 0.0; var amp = 0.5;
    for (var i = 0; i < 5; i++) { v += noise2(p) * amp; p = p * 2.03 + vec2f(1.7, 9.2); amp *= 0.5; }
    return v;
}
@fragment fn fs_sky(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let d = textureLoad(depth, vec2i(position.xy), 0);
    if (d < 1.0) { discard; }
    let ndc = vec2f((position.x - a.viewport.x) / a.viewport.z * 2.0 - 1.0,
                    1.0 - (position.y - a.viewport.y) / a.viewport.w * 2.0);
    let w = a.inverseViewProj * vec4f(ndc, 1.0, 1.0);
    let dir = normalize(w.xyz / w.w - a.camera.xyz);
    let up = dir.z;   // z para arriba en el mundo del 0.97k
    var sky = mix(a.sky0.rgb, a.sky1.rgb, smoothstep(-0.05, 0.55, up));
    // Bajo el horizonte (el borde del mapa): más oscuro, hacia el horizonte.
    sky = mix(sky, a.sky0.rgb * 0.6, smoothstep(0.0, -0.4, up));
    let sunDir = normalize(a.sun.xyz);
    let s = max(dot(dir, sunDir), 0.0);
    let clouds = a.sky1.w;
    let rain = a.sun.w;
    // Nubes proyectadas sobre un techo: más chicas cerca del horizonte.
    if (up > 0.0) {
        let uv = dir.xy / (up + 0.12) * 1.6 + vec2f(a.sunColor.w * 0.012, a.sunColor.w * 0.004);
        let n = fbm(uv);
        let cover = smoothstep(1.0 - clouds * 0.9 - 0.15, 1.05 - clouds * 0.6, n) * smoothstep(0.0, 0.18, up);
        let cloudColor = mix(vec3f(1.0, 0.98, 0.95) * mix(a.sky0.rgb, vec3f(1.0), 0.6), a.sky1.rgb * 0.7, rain);
        sky = mix(sky, cloudColor * (0.85 + 0.25 * s), cover * (0.75 + 0.25 * rain));
    }
    sky += a.sunColor.rgb * (pow(s, 600.0) * 2.0 + pow(s, 24.0) * 0.35 + pow(s, 4.0) * 0.12) * (1.0 - rain * 0.8);
    return vec4f(clamp(sky, vec3f(0.0), vec3f(1.0)), 1.0);
}
)WGSL";
bool EnsureAtmosphere() {
    const WGPUTextureFormat format = SurfaceFormat();
    const uint32_t samples = Samples();
    if (g.atmosphereMultiply && g.atmosphereFormat == format && g.atmosphereSamples == samples) return true;
    if (g.atmosphereMultiply) wgpuRenderPipelineRelease(g.atmosphereMultiply);
    if (g.atmosphereAdd) wgpuRenderPipelineRelease(g.atmosphereAdd);
    if (g.atmosphereSky) wgpuRenderPipelineRelease(g.atmosphereSky);
    if (g.atmospherePipelineLayout) wgpuPipelineLayoutRelease(g.atmospherePipelineLayout);
    if (g.atmosphereLayout) wgpuBindGroupLayoutRelease(g.atmosphereLayout);
    if (g.atmosphereModule) wgpuShaderModuleRelease(g.atmosphereModule);
    g.atmosphereMultiply = g.atmosphereAdd = g.atmosphereSky = nullptr;
    std::string code = AtmosphereShader;
    const char* type = samples > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d";
    code.replace(code.find("DEPTH_TYPE"), 10, type);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = WGPUStringView{code.c_str(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    g.atmosphereModule = wgpuDeviceCreateShaderModule(GetDevice(), &md);
    WGPUBindGroupLayoutEntry entries[2] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[0].texture.multisampled = samples > 1;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(State::Atmosphere);
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 2; ld.entries = entries;
    g.atmosphereLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &ld);
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 1; pd.bindGroupLayouts = &g.atmosphereLayout;
    g.atmospherePipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
    if (!g.atmosphereBuffer) g.atmosphereBuffer = CreateBuffer(sizeof(State::Atmosphere), WGPUBufferUsage_Uniform);
    auto pipeline = [&](const char* entry, WGPUBlendFactor src, WGPUBlendFactor dst) {
        WGPUBlendState blend{};
        blend.color = {WGPUBlendOperation_Add, src, dst};
        blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
        WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
        target.format = format;
        target.blend = &blend;
        target.writeMask = WGPUColorWriteMask_All;
        WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
        fragment.module = g.atmosphereModule;
        fragment.entryPoint = View(entry);
        fragment.targetCount = 1;
        fragment.targets = &target;
        WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
        d.layout = g.atmospherePipelineLayout;
        d.multisample.count = samples;
        d.vertex.module = g.atmosphereModule;
        d.vertex.entryPoint = View("vs");
        d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
        d.fragment = &fragment;
        return wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
    };
    g.atmosphereMultiply = pipeline("fs_multiply", WGPUBlendFactor_Zero, WGPUBlendFactor_Src);
    g.atmosphereAdd = pipeline("fs_add", WGPUBlendFactor_One, WGPUBlendFactor_One);
    g.atmosphereSky = pipeline("fs_sky", WGPUBlendFactor_One, WGPUBlendFactor_Zero);
    g.atmosphereFormat = format; g.atmosphereSamples = samples;
    return g.atmosphereMultiply && g.atmosphereAdd && g.atmosphereSky && g.atmosphereBuffer;
}
void EncodeAtmosphere(WGPUCommandEncoder encoder, unsigned width, unsigned height) {
    if (!g.depthView || !EnsureAtmosphere()) return;
    // Viewport de OpenGL (origen abajo) a píxeles desde arriba, dentro del cuadro.
    const int* v = g.atmosphereViewport;
    const float x = float(std::max(0, v[0]));
    const float w = std::min(float(v[2]), float(width) - x);
    const float top = float(int(height) - v[1] - v[3]);
    const float y = std::max(0.0f, top);
    const float h = std::min(float(v[3]) - (y - top), float(height) - y);
    if (w <= 0 || h <= 0) return;
    g.atmosphere.viewport[0] = float(v[0]); g.atmosphere.viewport[1] = top;
    g.atmosphere.viewport[2] = float(v[2]); g.atmosphere.viewport[3] = float(v[3]);
    wgpuQueueWriteBuffer(GetQueue(), g.atmosphereBuffer, 0, &g.atmosphere, sizeof(g.atmosphere));
    WGPUBindGroupEntry entries[2] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = g.depthView;
    entries[1].binding = 1; entries[1].buffer = g.atmosphereBuffer; entries[1].size = sizeof(State::Atmosphere);
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = g.atmosphereLayout; bd.entryCount = 2; bd.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(GetDevice(), &bd);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ColorTarget(color);
    color.loadOp = WGPULoadOp_Load;
    color.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderSetViewport(pass, x, y, w, h, 0, 1);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    if (g.atmosphere.sky0[3] > 0.5f) {
        wgpuRenderPassEncoderSetPipeline(pass, g.atmosphereSky);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    }
    if (g.atmosphere.params[0] > 0.5f) {
        wgpuRenderPassEncoderSetPipeline(pass, g.atmosphereMultiply);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
        wgpuRenderPassEncoderSetPipeline(pass, g.atmosphereAdd);
        wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    g.retiredGroups.push_back(group);
}

// ── Mar (prueba) ────────────────────────────────────────────────────────────
// Por píxel corta el rayo de la cámara contra el plano z = altura y lo dibuja
// sólo donde el plano queda delante de lo ya dibujado (la profundidad de la
// escena). Olas: suma de ondas direccionales (normales analíticas), Fresnel
// contra un cielo de atardecer y el brillo del sol. Sin geometría nueva.
const char* const WaterShader = R"WGSL(
struct Water { inverseViewProj: mat4x4f, viewProj: mat4x4f, camera: vec4f, params: vec4f, sun: vec4f, viewport: vec4f,
               lightCount: vec4f, lights: array<vec4f, 32>, ships: array<vec4f, 4>, tint: vec4f, sky: vec4f, sim: vec4f };
@group(0) @binding(0) var depth: DEPTH_TYPE;
@group(0) @binding(1) var<uniform> u: Water;
@group(0) @binding(2) var scene: texture_2d<f32>;
@group(0) @binding(3) var linearSampler: sampler;
@group(0) @binding(4) var waterMask: texture_2d<f32>;
@group(0) @binding(5) var waves: texture_2d<f32>;
// Pendiente de las ondas simuladas en (x, y); 0 fuera del área.
fn waveSlope(xy: vec2f) -> vec2f {
    if (u.sim.w < 0.5) { return vec2f(0.0); }
    let c = vec2i(floor((xy - u.sim.xy) / u.sim.z));
    let n = i32(textureDimensions(waves).x);
    if (c.x < 1 || c.y < 1 || c.x > n - 2 || c.y > n - 2) { return vec2f(0.0); }
    let hx = textureLoad(waves, c + vec2i(1, 0), 0).x - textureLoad(waves, c - vec2i(1, 0), 0).x;
    let hy = textureLoad(waves, c + vec2i(0, 1), 0).x - textureLoad(waves, c - vec2i(0, 1), 0).x;
    return vec2f(hx, hy) / (2.0 * u.sim.z);
}
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = vec2f(select(-1.0, 3.0, i == 1u), select(-1.0, 3.0, i == 2u));
    return vec4f(p, 0.0, 1.0);
}
fn worldAtPixel(s: vec2f, d: f32) -> vec3f {
    let ndc = vec2f((s.x - u.viewport.x) / u.viewport.z * 2.0 - 1.0,
                    1.0 - (s.y - u.viewport.y) / u.viewport.w * 2.0);
    let w = u.inverseViewProj * vec4f(ndc, d * 2.0 - 1.0, 1.0);
    return w.xyz / w.w;
}
fn worldAt(position: vec4f, d: f32) -> vec3f { return worldAtPixel(position.xy, d); }
// Punto del mundo -> píxel del cuadro (x, y); z < 0 si queda detrás de la cámara.
fn toPixel(q: vec3f) -> vec3f {
    let c = u.viewProj * vec4f(q, 1.0);
    if (c.w <= 0.0) { return vec3f(-1.0, -1.0, -1.0); }
    let n = c.xy / c.w;
    return vec3f(u.viewport.x + (n.x * 0.5 + 0.5) * u.viewport.z,
                 u.viewport.y + (0.5 - n.y * 0.5) * u.viewport.w, 1.0);
}
// Sombra de lo dibujado (cascos, velas, jarcia) sobre el agua: se avanza desde
// el punto del agua hacia el sol y se mira si algo de la profundidad del
// cuadro queda delante, a poca distancia del rayo. Sólo ve oclusores que están
// en pantalla, que en esta escena son todos.
)WGSL"
// MSVC limita cada literal a 16 KB: el shader va en dos partes.
R"WGSL(
// 1 si debajo de la luz hay algo sólido antes del agua (la cubierta de un
// barco): baja por la vertical y ve si algún punto queda dentro de lo dibujado.
fn overHull(lp: vec3f, level: f32) -> f32 {
    let cam = u.camera.xyz; let dims = vec2f(textureDimensions(depth));
    for (var k = 0; k < 12; k++) {
        let q = vec3f(lp.xy, mix(lp.z, level + 30.0, (f32(k) + 0.5) / 12.0));
        let s = toPixel(q);
        if (s.z < 0.0 || s.x < 0.0 || s.y < 0.0 || s.x >= dims.x || s.y >= dims.y) { continue; }
        let ds = textureLoad(depth, vec2i(s.xy), 0); if (ds >= 1.0) { continue; }
        let gap = length(q - cam) - length(worldAtPixel(s.xy, ds) - cam);
        if (gap > 5.0 && gap < 400.0) { return 1.0; }
    }
    return 0.0;
}
fn sunShadow(p: vec3f, toSun: vec3f, jitter: f32) -> f32 {
    let cam = u.camera.xyz;
    let dims = vec2f(textureDimensions(depth));
    var shade = 0.0;
    for (var k = 0; k < 32; k++) {
        let f = (f32(k) + jitter) / 32.0;
        let dist = 8.0 + f * f * 900.0;
        let q = p + toSun * dist;
        let s = toPixel(q);
        if (s.z < 0.0 || s.x < 0.0 || s.y < 0.0 || s.x >= dims.x || s.y >= dims.y) { break; }
        let ds = textureLoad(depth, vec2i(s.xy), 0);
        if (ds >= 1.0) { continue; }
        let dq = length(q - cam);
        let dsc = length(worldAtPixel(s.xy, ds) - cam);
        // Delante del rayo pero cerca de él: lo que está mucho más cerca de la
        // cámara (el barco propio, la soga) no tapa este sol.
        let gap = dq - dsc;
        let thick = 30.0 + dist * 0.12;
        // Penumbra: la sombra se aclara cuanto más lejos está lo que la proyecta.
        let soft = 1.0 - 0.7 * smoothstep(150.0, 900.0, dist);
        shade = max(shade, smoothstep(1.0, 12.0, gap) * (1.0 - smoothstep(thick, thick * 1.6, gap)) * soft);
        if (shade > 0.99) { break; }
    }
    return shade;
}
// Color de la copia del cuadro con un desenfoque de 5 muestras (radio en px).
fn sceneAt(px: vec2f, blur: f32) -> vec3f {
    let size = vec2f(textureDimensions(scene));
    var c = vec3f(0.0);
    c += textureSampleLevel(scene, linearSampler, px / size, 0.0).rgb;
    c += textureSampleLevel(scene, linearSampler, (px + vec2f(blur, 0.0)) / size, 0.0).rgb;
    c += textureSampleLevel(scene, linearSampler, (px - vec2f(blur, 0.0)) / size, 0.0).rgb;
    c += textureSampleLevel(scene, linearSampler, (px + vec2f(0.0, blur)) / size, 0.0).rgb;
    c += textureSampleLevel(scene, linearSampler, (px - vec2f(0.0, blur)) / size, 0.0).rgb;
    return c * 0.2;
}
// Reflejo continuo: el rayo reflejado se lleva hasta la pared pintada del
// fondo (params.z) y se lee lo que se ve en esa dirección en la copia del
// cuadro: el cielo pintado, y los barcos y el sol que lo tapan. Exacto para el
// fondo y aproximado para los barcos, pero sin búsqueda de choques, así que no
// deja bordes ni manchas. a = cuánto vale (se apaga si cae fuera del cuadro).
fn reflection(h: vec3f, r: vec3f) -> vec4f {
    let reach = max((u.params.z - h.y) / max(r.y, 0.05), 50.0);
    let px = toPixel(h + r * reach);
    if (px.z < 0.0) { return vec4f(0.0); }
    let size = vec2f(textureDimensions(scene));
    let lo = max(u.viewport.xy, vec2f(0.0)) + 2.0;
    let hi = min(u.viewport.xy + u.viewport.zw, size) - 2.0;
    let c = clamp(px.xy, lo, hi);
    let outside = length(px.xy - c);
    let blur = 2.5 + reach * 0.006;
    return vec4f(sceneAt(c, blur), 1.0 - smoothstep(0.0, 80.0, outside));
}
// Pendiente (dh/dx, dh/dy) de la suma de ondas. xy = dirección, z = longitud
// de onda en unidades del mundo, w = pendiente máxima de la onda.
fn slope(p: vec2f, t: f32, detail: f32) -> vec2f {
    var waves = array<vec4f, 8>(
        // Mar de fondo: larga y lenta; la misma que mece a los barcos (en el
        // cliente, Gfx::WaterSwell). Si se cambia acá hay que cambiarla allá.
        vec4f( 0.35,  0.94, 900.0, 0.035),
        vec4f( 0.80,  0.60, 210.0, 0.060),
        vec4f(-0.45,  0.89, 130.0, 0.055),
        vec4f( 0.96, -0.28,  80.0, 0.050),
        vec4f(-0.88, -0.47,  47.0, 0.045),
        vec4f( 0.21,  0.98,  29.0, 0.040),
        vec4f( 0.62, -0.78,  17.0, 0.035),
        vec4f(-0.30,  0.95,  10.0, 0.030));
    var g = vec2f(0.0);
    for (var i = 0; i < 8; i++) {
        let w = waves[i];
        let d = normalize(w.xy);
        let k = 6.2831853 / w.z;
        let omega = sqrt(300.0 * k);
        // Las ondas cortas se apagan con la distancia (si no, titilan).
        let fade = select(1.0, detail, w.z < 60.0);
        g += d * (w.w * fade * cos(k * dot(d, p) - omega * t));
    }
    return g;
}
// Olas de proa (patrón de Kelvin): de la proa de cada barco salen dos brazos
// en V a ~19.5 grados del rumbo, con crestas cortas inclinadas, y por dentro
// olas transversales más largas. Devuelve la pendiente (xy) y la espuma (z) de
// las crestas cerca de la proa.
fn kelvin(p: vec2f, t: f32) -> vec3f {
    var g = vec2f(0.0);
    var foam = 0.0;
    let count = i32(u.lightCount.y);
    for (var i = 0; i < 4; i++) {
        if (i >= count) { break; }
        let s = u.ships[i];
        let fwd = s.zw;
        let side = vec2f(-fwd.y, fwd.x);
        let rel = p - s.xy;
        let a = -dot(rel, fwd);              // distancia detrás de la proa
        if (a <= 0.0 || a > 2200.0) { continue; }
        let b = dot(rel, side);
        let sb = select(-1.0, 1.0, b >= 0.0);
        let ab = abs(b);
        // Brazos divergentes.
        let arm = ab - 0.354 * a;
        let sig = 10.0 + 0.045 * a;
        // Las crestas no son líneas largas: son tramos cortos escalonados a lo
        // largo del brazo, que aparecen y se apagan.
        let segs = 0.5 + 0.5 * sin(a * 0.021 + ab * 0.013 + sb * 1.3 - t * 0.7);
        let env = exp(-arm * arm / (2.0 * sig * sig)) * exp(-a / 900.0) * smoothstep(0.0, 80.0, a) * (0.35 + 0.65 * segs);
        let kd = 6.2831853 / 42.0;
        let ph = kd * (0.57 * a + 0.82 * ab) - 2.4 * t;
        let gd = 0.10 * env * cos(ph);
        // Ejes locales: a crece hacia atrás (-fwd) y b hacia el costado.
        g += gd * (0.57 * -fwd + 0.82 * sb * side);
        foam += env * smoothstep(0.6, 1.0, sin(ph)) * exp(-a / 260.0);
        // Transversales, dentro de la V.
        let inside = smoothstep(0.0, 60.0, -arm);
        let kt = 6.2831853 / 110.0;
        g += -fwd * (0.022 * inside * exp(-a / 700.0) * cos(kt * a - 1.6 * t));
    }
    return vec3f(g, foam);
}
)WGSL"
// Tercera parte del shader (límite de 16 KB por literal de MSVC).
R"WGSL(
// Cuánta agua hay en (x, y) del mundo, con la misma regla que el terreno:
// cada tile se pinta entero con la textura de su vértice (x, y) (R: si es
// agua) y encima la capa 2 (G: si es agua) mezclada con el alfa de cada
// vértice (B, interpolado). Así coincide con el agua original, también en las
// paredes que el mapa pinta con agua (debajo de los puentes).
// R lleva los dos bits del tile: base (1) y capa 2 (2), en pasos de 85.
// R: bits del tile en pasos de 36 (1 = agua, 2 = agua en la capa 2,
// 4 = cauce hondo).
fn tileBits(xy: vec2f) -> u32 {
    let t = textureLoad(waterMask, clamp(vec2i(floor(xy / 100.0)), vec2i(0), vec2i(255)), 0).r;
    return u32(round(t * 255.0 / 36.0));
}
// Cuánta agua pinta el mapa en (x, y): la base del tile y la capa 2 mezcladas
// con el alfa de cada vértice (B), igual que el terreno. Sale con la orilla
// fundida que pintó el mapa.
fn waterAt(xy: vec2f) -> f32 {
    let bits = tileBits(xy);
    let a = textureSampleLevel(waterMask, linearSampler, (xy / 100.0 + 0.5) / 256.0, 0.0).b;
    return mix(f32(bits & 1u), f32((bits >> 1u) & 1u), a);
}
)WGSL"
// Quinta parte del shader (límite de 16 KB por literal de MSVC).
R"WGSL(
// Altura de la superficie del agua en (x, y) (G: z/2 + 20; 0 = sin agua, que
// da un plano muy hundido). Interpolada entre vértices.
fn levelAt(xy: vec2f) -> f32 {
    let g = textureSampleLevel(waterMask, linearSampler, (xy / 100.0 + 0.5) / 256.0, 0.0).g;
    // + 8: el plano queda por encima de los tiles de agua aunque el nivel se
    // guarde redondeado de a 2.
    return (g * 255.0 - 20.0) * 2.0 + 8.0;
}
// Superficie del agua redibujada: los mismos dos triángulos por tile que el
// terreno (0-1-2, 0-2-3). Con la bilineal el suelo asomaba en triángulos. Va 5
// encima y no 8: más alto tapa los nenúfares.
fn redrawLevelAt(xy: vec2f) -> f32 {
    let q = clamp(xy / 100.0, vec2f(0.0), vec2f(254.999));
    let i = vec2i(floor(q));
    let f = q - floor(q);
    let a = textureLoad(waterMask, i, 0).g;
    let b = textureLoad(waterMask, i + vec2i(1, 0), 0).g;
    let c = textureLoad(waterMask, i + vec2i(1, 1), 0).g;
    let d = textureLoad(waterMask, i + vec2i(0, 1), 0).g;
    let g = select(a + (c - d) * f.x + (d - a) * f.y, a + (b - a) * f.x + (c - b) * f.y, f.x >= f.y);
    return (g * 255.0 - 20.0) * 2.0 + 5.0;
}
// 0..1: cuánto de plano es el suelo en este píxel. El agua del mapa es plana;
// la máscara, al interpolar entre vértices, se mete en la barranca de la orilla
// y ahí no tiene que haber agua. La normal sale de los píxeles vecinos (sin
// derivadas: después de un discard no se pueden usar).
fn flatAt(px: vec2f, p: vec3f) -> f32 {
    let dims = vec2f(textureDimensions(depth));
    let qx = clamp(px + vec2f(2.0, 0.0), vec2f(0.0), dims - 1.0);
    let qy = clamp(px + vec2f(0.0, 2.0), vec2f(0.0), dims - 1.0);
    let dx = textureLoad(depth, vec2i(qx), 0);
    let dy = textureLoad(depth, vec2i(qy), 0);
    if (dx >= 1.0 || dy >= 1.0) { return 0.0; }
    let n = cross(worldAtPixel(qx, dx) - p, worldAtPixel(qy, dy) - p);
    let len = length(n);
    if (len < 1e-4) { return 0.0; }
    return smoothstep(0.90, 0.98, abs(n.z) / len);
}
)WGSL"
// Cuarta parte del shader (límite de 16 KB por literal de MSVC).
R"WGSL(
// Reflejo de lo dibujado (murallas, puentes, árboles, la orilla): el rayo
// reflejado avanza por el mundo y se proyecta al cuadro; el primer punto que
// queda detrás de lo dibujado es lo que se ve. a = cuánto vale (se apaga si no
// encuentra nada o cae cerca del borde del cuadro).
fn worldReflection(p: vec3f, r: vec3f, jitter: f32) -> vec4f {
    let cam = u.camera.xyz;
    let dims = vec2f(textureDimensions(depth));
    let size = vec2f(textureDimensions(scene));
    var prev = 0.0;
    for (var k = 0; k < 28; k++) {
        let f = (f32(k) + jitter) / 28.0;
        let t = 10.0 + f * f * 2400.0;
        let q = p + r * t;
        let s = toPixel(q);
        if (s.z < 0.0 || s.x < 1.0 || s.y < 1.0 || s.x >= dims.x - 1.0 || s.y >= dims.y - 1.0) { break; }
        let ds = textureLoad(depth, vec2i(s.xy), 0);
        if (ds >= 1.0) { prev = t; continue; }
        let gap = length(q - cam) - length(worldAtPixel(s.xy, ds) - cam);
        if (gap > 0.0 && gap < 40.0 + t * 0.2) {
            // Afinar entre el paso anterior y este.
            var lo = prev;
            var hi = t;
            for (var j = 0; j < 4; j++) {
                let mid = 0.5 * (lo + hi);
                let qm = p + r * mid;
                let sm = toPixel(qm);
                let dm = textureLoad(depth, vec2i(clamp(sm.xy, vec2f(0.0), dims - 1.0)), 0);
                if (dm < 1.0 && length(qm - cam) > length(worldAtPixel(sm.xy, dm) - cam)) { hi = mid; } else { lo = mid; }
            }
            let sh = toPixel(p + r * hi);
            let edge = min(min(sh.x, dims.x - sh.x), min(sh.y, dims.y - sh.y));
            let fade = smoothstep(0.0, 0.06 * dims.y, edge) * (1.0 - smoothstep(0.6, 1.0, f));
            return vec4f(textureSampleLevel(scene, linearSampler, sh.xy / size, 0.0).rgb, fade);
        }
        prev = t;
    }
    return vec4f(0.0);
}
// Patrón de cáusticas: dos capas de celdas que se deforman con el tiempo; la
// línea entre celdas (donde la luz se junta) queda clara. 0..1.
fn q2c(xy: vec2f, t: f32) -> f32 {
    var c = 0.0;
    for (var k = 0; k < 2; k++) {
        let o = f32(k) * 1.7;
        var q = xy * (1.0 + 0.6 * f32(k)) + vec2f(sin(t * 0.4 + o), cos(t * 0.33 + o)) * 1.3;
        q += vec2f(sin(q.y * 1.3 + t * 0.7), cos(q.x * 1.1 - t * 0.6)) * 0.45;
        let v = abs(sin(q.x) * sin(q.y));
        c += pow(1.0 - v, 6.0);
    }
    return clamp(c * 0.6, 0.0, 1.0);
}
fn worldWater(position: vec4f) -> vec4f {
    let d = textureLoad(depth, vec2i(position.xy), 0);
    if (d >= 1.0) { discard; }
    let ground = worldAt(position, d);
    let cam = u.camera.xyz;
    let t = u.params.y;
    // El agua es un plano a la altura de su zona (como el mar del login): el
    // rayo de la cámara se corta con él y hay agua si el suelo queda debajo.
    // Así la orilla es la línea donde el plano corta el terreno (no los bordes
    // de los tiles) y un fondo en pendiente queda tapado por una superficie
    // plana. El nivel depende de dónde cae el rayo: se ajusta dos veces.
    let ray = normalize(ground - cam);
    // Redibujada: se compara en la vertical del píxel. La superficie sigue la
    // pendiente y el ajuste por el rayo la pasaba por debajo del suelo.
    let redrawn = u.tint.w > 0.5;
    var level = levelAt(ground.xy);
    if (redrawn) { level = redrawLevelAt(ground.xy); }
    var p = ground;
    var onPlane = false;
    if (ray.z < -0.001) {
        var tt = (level - cam.z) / ray.z;
        if (!redrawn) {
            level = levelAt((cam + ray * tt).xy);
            tt = (level - cam.z) / ray.z;
        }
        if (tt > 0.0 && tt < length(ground - cam) && level > ground.z) {
            onPlane = true;
            p = cam + ray * tt;
        }
    }
    // Lo que el mapa pinta con agua y queda por encima del plano (las paredes
    // bajo los puentes, el borde de un estanque) va con el color del agua, sin
    // olas: en las paredes siempre, en lo casi plano se apaga al subir.
    let m = waterAt(ground.xy);
    let flatGround = flatAt(position.xy, ground);
    // Cauce hondo (la superficie sube sobre un fondo tallado) o agua plana
    // (charcos, arroyos a ras del suelo).
    let deepTile = (tileBits(ground.xy) & 4u) != 0u;
    var coverage = 0.0;
    var flat = 0.0;
    var wall = false;
    if (onPlane) {
        // Se funde con la orilla donde el agua es poco profunda.
        // El plano va 8 por encima del agua del mapa. Sobre agua pintada se
        // cubre enseguida; sobre suelo seco (arena, pasto al nivel del agua)
        // sólo si está bien por debajo, si no se desborda sobre la orilla.
        // En un cauce hondo el agua cubre todo lo que queda debajo (barrancas,
        // el pie de las murallas). En lo plano (arroyos, charcos) sólo donde el
        // mapa pone agua, con la orilla redondeada: si no, cualquier pozo del
        // suelo junto al arroyo se volvía un charco.
        let depthBelow = level - ground.z;
        if (deepTile) {
            coverage = select(smoothstep(8.0, 16.0, depthBelow), smoothstep(0.0, 4.0, depthBelow), waterAt(ground.xy) > 0.5);
        } else if (u.tint.w > 0.5) {
            // Redibujada: orilla suave por el agua que pinta el mapa (B).
            let wet = textureSampleLevel(waterMask, linearSampler, (ground.xy / 100.0 + 0.5) / 256.0, 0.0).b;
            // Corta bajo: un charco de un tile o con pasto encima apenas llega
            // a 0.25 en sus vértices y desaparecía.
            coverage = smoothstep(0.12, 0.35, wet) * smoothstep(0.0, 4.0, depthBelow);
        } else {
            coverage = waterAt(ground.xy) * smoothstep(0.0, 4.0, depthBelow);
        }
        flat = 1.0;
    }
    // Fuera del plano no se pinta nada: el pase va después de los objetos, y
    // lo que queda por encima del agua (barrancas, murallas, el puente) es lo
    // que es. Los tiles de agua que el mapa pinta en seco los cambia de
    // textura el terreno (WaterProfiles::WallTexture).
    if (coverage < 0.01) { discard; }
    let dir = ray;
    let dist = length(p - cam);
    // Las ondas largas son ocho sumas de senos: de lejos, cuando se apagan las
    // cortas, quedan franjas paralelas regulares. Se tuercen un poco (el punto
    // se desplaza con un campo lento) y de lejos se aplanan, así no se ven.
    let warp = vec2f(sin(p.y * 0.0041 + t * 0.11) + sin(p.x * 0.0029 - t * 0.07) + 0.6 * sin((p.x + p.y) * 0.011 + t * 0.2),
                     cos(p.x * 0.0037 - t * 0.09) + cos(p.y * 0.0023 + t * 0.05) + 0.6 * cos((p.x - p.y) * 0.013 - t * 0.17)) * 160.0;
    let detail = max(1.0 - smoothstep(1200.0, 3500.0, dist), 0.35);
    let far = smoothstep(1500.0, 4000.0, dist);
    var g = slope(p.xy + warp, t, detail) * u.sky.w * flat * (1.0 - 0.6 * far);
    // Ondas que levanta lo que toca el agua (simuladas).
    let ws = waveSlope(p.xy) * flat;
    g += ws * 2.5;
    // Lo que nada (luces con w = 2) levanta ondas en anillo y espuma en el
    // borde donde corta el agua.
    var swimFoam = 0.0;
    for (var i = 0; i < 16; i++) {
        if (i >= i32(u.lightCount.x)) { break; }
        if (u.lights[i * 2 + 1].w < 1.5) { continue; }
        let off = p.xy - u.lights[i * 2].xy;
        let dd = length(off);
        if (dd > 420.0) { continue; }
        // Sólo un borde suave donde el cuerpo corta el agua, sin anillos.
        swimFoam = max(swimFoam, (1.0 - smoothstep(20.0, 55.0, dd)) * 0.6);
    }
    let n = normalize(vec3f(-g.x, -g.y, 1.0));
    let cosv = max(dot(n, -dir), 0.0);
    let fresnel = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
    // Del agua original no se toma nada de la imagen: su textura se repite
    // por tile y cualquier muestra (aunque se promedie) deja ver la cuadrícula.
    // La luz del lugar (sombras, día y noche, los fuegos) sale de la luz del
    // terreno por vértice (A de la máscara), que es suave; el color, del perfil.
    let vertexLight = textureSampleLevel(waterMask, linearSampler, (p.xy / 100.0 + 0.5) / 256.0, 0.0).a * 2.0;
    let light = vertexLight * 0.3;
    // Las paredes pintadas con agua (bajo los puentes) están en sombra: más
    // oscuras que la superficie, si no quedan como una franja clara.
    let deep = u.tint.rgb * (0.25 + 1.1 * vertexLight) * select(1.0, 0.5, wall);
    // Sobre el cauce (el terreno se hunde al dibujarlo) se ve el fondo a
    // través del agua, corrido por las olas, y se pierde con la profundidad.
    var body = deep;
    if (onPlane) {
        let depthW = level - ground.z;
        let size = vec2f(textureDimensions(scene));
        let dims = vec2f(textureDimensions(depth));
        var at = position.xy;
        let sh = clamp(position.xy + g * 0.05 * u.viewport.w * clamp(depthW / 60.0, 0.0, 1.0), vec2f(0.0), dims - 1.0);
        let dsh = textureLoad(depth, vec2i(sh), 0);
        if (dsh < 1.0) {
            let q = worldAtPixel(sh, dsh);
            if (levelAt(q.xy) > q.z) { at = sh; }
        }
        var bed = textureSampleLevel(scene, linearSampler, at / size, 0.0).rgb;
        // Cáusticas: la luz que las olas concentran en el fondo, como redes
        // claras que se mueven. Se apagan con la profundidad y en la sombra.
        let cq = q2c(p.xy * 0.018 + g * 6.0, t);
        bed *= 1.0 + cq * 1.6 * exp(-depthW * 0.008) * (0.4 + vertexLight);
        // Poco hondo: el fondo se ve claro, teñido de turquesa; hondo: el azul
        // del agua. Más transparente que antes (desde arriba es lo que se ve).
        let shallow = bed * vec3f(0.78, 1.0, 0.94) * 1.1;
        body = mix(deep, shallow, exp(-depthW * 0.010));
    }
    // En el agua plana no hay cauce que mostrar: el cuerpo es el agua que pinta
    // el mapa (cada mapa con su estilo, como el azul de Noria), movida por las
    // olas. Lo nuevo sólo le suma reflejo, ondas y sol; así no queda una lámina
    // encima ni bordes contra el agua vieja.
    if (!deepTile && u.tint.w > 0.5) {
        let sceneSize = vec2f(textureDimensions(scene));
        let wob = clamp(position.xy + g * 0.02 * u.viewport.w, vec2f(0.0), sceneSize - 1.0);
        var under = textureSampleLevel(scene, linearSampler, wob / sceneSize, 0.0).rgb;
        if (u.tint.w > 0.5) {
            // Redibujada: el suelo (sin el agua vieja) a través de agua poco
            // honda del color del mapa, con cáusticas.
            under *= 1.0 + q2c(p.xy * 0.018 + g * 6.0, t) * 1.2 * (0.4 + vertexLight);
            body = mix(u.tint.rgb * (0.6 + 1.6 * vertexLight), under * vec3f(0.70, 0.88, 1.0), 0.45);
        } else {
            body = under;
        }
    }
    // Cielo: más claro hacia el horizonte del reflejo.
    let r = reflect(dir, n);
    // Reflejo de lo que hay sobre el agua, con la normal suavizada (con la de
    // las olas titila) y corrido por las olas.
    let nr = normalize(vec3f(-g.x * 0.35, -g.y * 0.35, 1.0));
    // Cielo como en el login: claro en el horizonte, más hondo arriba y algo
    // más cálido hacia el sol. Con la normal suavizada, así no deja franjas.
    let rs = reflect(dir, nr);
    // Sol del agua: la cámara del juego mira hacia abajo y el sol del mapa casi
    // nunca cae en el reflejo. Se lo pone delante de la cámara, un poco por
    // encima del reflejo del centro de la pantalla, así su brillo cruza el agua
    // a la vista como en el login.
    let center = worldAtPixel(u.viewport.xy + u.viewport.zw * 0.5, 1.0);
    let ahead = normalize(center - cam);
    let toSunView = normalize(vec3f(normalize(ahead.xy + vec2f(1e-5)) * 0.8, -ahead.z * 0.75));
    let toSunSky = toSunView;
    let towardSun = pow(max(dot(normalize(rs.xy + vec2f(1e-5)), normalize(toSunSky.xy + vec2f(1e-5))), 0.0), 6.0);
    let horizon = mix(u.sky.rgb * 1.15, u.sky.rgb * vec3f(1.25, 1.12, 0.95), towardSun);
    let sky = mix(horizon, u.sky.rgb * vec3f(0.45, 0.55, 0.70), sqrt(clamp(rs.z, 0.0, 1.0)));
    let jitter = fract(52.982918 * fract(dot(position.xy, vec2f(0.06711056, 0.00583715))));
    let mirror = select(vec4f(0.0), worldReflection(p, reflect(dir, nr), jitter), onPlane);
    let reflected = mix(sky, mirror.rgb, mirror.a);
    // Con la cámara del juego el agua se ve casi siempre en ángulo rasante: con
    // todo el Fresnel cada cresta refleja el cielo y quedan franjas. Se modera;
    // lo que refleja algo del cuadro pesa un poco más.
    let weight = clamp(fresnel * 1.1 + 0.10 + mirror.a * 0.25, 0.0, 0.8) * select(1.0, 0.6, !deepTile && u.tint.w > 0.5);
    var color = mix(body, reflected, weight * flat);
    // Sol: dirección fija del mapa (u.sun), brillo en las crestas.
    let toSun = toSunView;
    // Los destellos con una normal intermedia: con la de las olas siguen sus
    // crestas, que son sumas regulares, y quedan bandas en zigzag.
    let rg = reflect(dir, normalize(mix(n, nr, 0.5)));
    let s = max(dot(rg, toSun), 0.0);
    // La franja con una normal casi plana: con la de las olas grandes cada
    // ondulación queda marcada como una curva de nivel. La textura la ponen
    // los destellos chicos.
    let rf = reflect(dir, normalize(vec3f(-g.x * 0.08, -g.y * 0.08, 1.0)));
    let sPath = max(dot(rf, toSun), 0.0);
    // Franja ancha y suave, la columna de luz, y los destellos de las olas.
    let sunLight = vec3f(1.0, 0.92, 0.78) * (0.35 + light);
    // Los destellos no siguen la forma de las olas (un umbral sobre olas suaves
    // dibuja líneas): son puntos de grano fino que titilan, más densos donde la
    // ola mira al sol.
    let cell = floor(p.xy * 0.3 + g * 18.0 + vec2f(t * 0.9, t * 0.6));
    let glitter = pow(fract(sin(dot(cell, vec2f(12.9898, 78.233))) * 43758.547), 45.0);
    // Sólo dentro de la franja del sol, y de lejos se apagan: en ángulo rasante
    // las celdas se estiran en rayitas.
    let near = 1.0 - smoothstep(900.0, 2200.0, dist);
    color += sunLight * (pow(sPath, 16.0) * 0.13 + pow(sPath, 70.0) * 0.32
           + pow(s, 20.0) * pow(sPath, 8.0) * glitter * 1.2 * near) * flat;
    // Fuegos del mapa reflejados: como los faroles del login, una columna de
    // destellos que nace en el agua bajo la llama (top) y se estira hacia su
    // punto espejado (mir). El nivel del agua es el de este píxel.
    let lampCount = i32(u.lightCount.x);
    for (var i = 0; i < 16; i++) {
        if (i >= lampCount) { break; }
        let lp = u.lights[i * 2].xyz;
        let lc = u.lights[i * 2 + 1].rgb;
        if (u.lights[i * 2 + 1].w > 1.5 || lp.z <= p.z) { continue; }
        let top = toPixel(vec3f(lp.xy, p.z));
        let mir = toPixel(vec3f(lp.xy, 2.0 * p.z - lp.z));
        if (top.z < 0.0 || mir.z < 0.0) { continue; }
        let span = max(mir.y - top.y, 8.0);
        let along = (position.y - top.y) / span;
        let cx = mix(top.x, mir.x, clamp(along, 0.0, 1.0));
        let width = 0.005 * u.viewport.w * (0.8 + 1.5 * clamp(along, 0.0, 1.0));
        let dx = position.x + g.x * 0.015 * u.viewport.w - cx;
        let profile = smoothstep(-0.05, 0.05, along) * (1.0 - smoothstep(0.35, 1.0, along));
        let ph = f32(i) * 1.7;
        let wa = 0.5 + 0.5 * sin(p.y * 0.55 + g.y * 90.0 + t * 3.1 + ph);
        let wb = 0.5 + 0.5 * sin(p.x * 0.80 + g.x * 70.0 - t * 2.2 + ph * 2.3);
        let sparkle = pow(wa * wb, mix(1.5, 4.0, clamp(along, 0.0, 1.0)));
        let core = exp(-dx * dx / (width * width));
        let glow = exp(-dx * dx / (4.0 * width * width)) * 0.15;
        color += lc * (core * sparkle * 1.2 + glow) * profile * 0.6 * flat;
    }
    // Las ondas simuladas se iluminan solas (sobre el agua oscura el reflejo
    // apenas cambia): la cara que mira al sol se aclara, la otra se oscurece,
    // y las crestas brillan.
    let wsLen = length(ws);
    let facing = dot(ws, normalize(u.sun.xy + vec2f(0.0001)));
    color *= 1.0 + clamp(facing * 2.5, -0.45, 0.6);
    color += vec3f(0.75, 0.82, 0.88) * (0.25 + light) * smoothstep(0.05, 0.35, wsLen) * 0.5;
    // Espuma en la orilla: donde la máscara baja, ondas que van y vienen.
    color = mix(color, vec3f(0.70, 0.76, 0.80) * (0.6 + light), swimFoam * 0.3 * flat);
    let edge = (1.0 - smoothstep(2.0, 14.0, level - ground.z)) * flat;
    let ripple = 0.5 + 0.5 * sin(dot(p.xy, vec2f(0.045, 0.037)) + t * 1.6 + g.x * 25.0);
    color = mix(color, vec3f(0.85, 0.88, 0.90) * (0.5 + light), edge * ripple * 0.35);
    return vec4f(color, coverage);
}
)WGSL"
// Sexta parte del shader (límite de 16 KB por literal de MSVC).
R"WGSL(
@fragment fn fs(@builtin(position) position: vec4f) -> @location(0) vec4f {
    if (u.params.w > 0.5) { return worldWater(position); }
    let cam = u.camera.xyz;
    let far = worldAt(position, 1.0);
    let dir = normalize(far - cam);
    if (dir.z > -0.0005) { discard; }
    let t = (u.params.x - cam.z) / dir.z;
    if (t <= 0.0) { discard; }
    let d = textureLoad(depth, vec2i(position.xy), 0);
    // Lo ya dibujado delante del plano (cascos, la pared del fondo) tapa el
    // agua. La transición contra el fondo la hace la bruma, más abajo.
    if (d < 1.0 && length(worldAt(position, d) - cam) < t) { discard; }
    let hit = cam + dir * t;
    let detail = 1.0 - smoothstep(350.0, 1000.0, t);
    let wake3 = kelvin(hit.xy, u.params.y);
    let g = slope(hit.xy, u.params.y, detail) + wake3.xy;
    let n = normalize(vec3f(-g.x, -g.y, 1.0));
    let v = -dir;
    let cosv = max(dot(n, v), 0.0);
    let fresnel = 0.02 + 0.98 * pow(1.0 - cosv, 5.0);
    let r = reflect(dir, n);
    let toSun = normalize(u.sun.xyz - hit);
    // Cielo de atardecer: horizonte cálido, cenit oscuro, más claro hacia el sol.
    let up = clamp(r.z, 0.0, 1.0);
    let towardSun = pow(max(dot(normalize(r.xy + vec2f(1e-5)), normalize(toSun.xy + vec2f(1e-5))), 0.0), 6.0);
    let horizon = mix(vec3f(0.55, 0.28, 0.24), vec3f(1.0, 0.62, 0.36), towardSun);
    let sky = mix(horizon, vec3f(0.10, 0.08, 0.20), sqrt(up));
    let deep = vec3f(0.035, 0.040, 0.055);
    // Reflejo de lo que hay sobre el agua (barcos, sol, el cielo pintado), con
    // la normal suavizada: con la de las olas el reflejo titila.
    let nr = normalize(vec3f(-g.x * 0.3, -g.y * 0.3, 1.0));
    let mirror = reflection(hit, reflect(dir, nr));
    // Algo más espejado que el Fresnel físico: el mar quieto del atardecer.
    let mirrorWeight = clamp(fresnel * 1.3 + 0.08, 0.0, 1.0);
    var color = mix(deep, mix(sky, mirror.rgb, mirror.a), mirrorWeight);
    // Bruma de atardecer: al acercarse a la pared pintada (el "horizonte" de la
    // escena) el agua se funde con el color del cielo justo encima, así no
    // queda una línea entre el mar y el fondo. Más cálida hacia el sol.
    let viewSun = pow(max(dot(normalize(dir.xy + vec2f(1e-5)), normalize(toSun.xy + vec2f(1e-5))), 0.0), 8.0);
    let hazeColor = mix(vec3f(0.20, 0.13, 0.12), vec3f(0.72, 0.36, 0.14), viewSun);
    // La mezcla con el fondo se mide en píxeles por debajo de donde el agua
    // toca la pared: en distancia del mundo, a lo lejos se comprime en pocos
    // píxeles y queda una franja con corte.
    let shore = toPixel(vec3f(hit.x, u.params.z, u.params.x));
    let below = (position.y - shore.y) / u.viewport.w;
    let haze = select(0.0, 1.0 - smoothstep(0.0, 0.12, below), shore.z > 0.0);
    // El brillo del sol sigue visible a través de la bruma, algo atenuado.
    // Camino del sol: una franja continua (normal suavizada) y encima los
    // destellos de las olas, más suaves.
    let s = max(dot(r, toSun), 0.0);
    let sPath = max(dot(reflect(dir, nr), toSun), 0.0);
    let glint = vec3f(1.0, 0.72, 0.45) * (pow(sPath, 60.0) * 0.45 + pow(s, 150.0) * 0.8 + pow(s, 25.0) * 0.15);
    // Contra el fondo el agua se funde con el mar pintado original que tiene
    // debajo (sigue en la copia del cuadro): así el horizonte no deja línea.
    let sceneSize = vec2f(textureDimensions(scene));
    let original = textureSampleLevel(scene, linearSampler, position.xy / sceneSize, 0.0).rgb;
    // El mar pintado es azul; se lleva al tono del agua nueva para no dejar
    // una franja de otro color.
    let originalLum = dot(original, vec3f(0.30, 0.55, 0.15));
    let back = mix(hazeColor, mix(color, vec3f(originalLum) * vec3f(1.15, 1.0, 0.9), 0.5), 0.75);
    color = mix(color, back, haze);
    // Estela de los barcos: es la parte clara del mar pintado original. Se toma
    // de la copia, movida por las olas y desaturada, en lugar de redibujarla
    // encima con su azul saturado.
    let wobble = position.xy + g * 40.0;
    let wake = textureSampleLevel(scene, linearSampler, wobble / sceneSize, 0.0).rgb;
    let wakeLum = dot(wake, vec3f(0.30, 0.55, 0.15));
    // El mar pintado ronda 0.10 de luminancia y la estela llega a ~0.25.
    let foam = smoothstep(0.11, 0.24, wakeLum) * (1.0 - haze);
    let foamColor = mix(wake, vec3f(wakeLum) * vec3f(0.92, 0.97, 1.0), 0.5) * 1.6;
    // Sombra de los barcos: tapa el brillo del sol y oscurece un poco el agua
    // y la espuma. El ruido por píxel en el inicio del rayo evita escalones.
    let jitter = fract(52.982918 * fract(dot(position.xy, vec2f(0.06711056, 0.00583715))));
    // El sol pintado está casi en el horizonte: con su altura real las sombras
    // cruzarían todo el mar. Se proyectan como con el sol a ~28 grados.
    let shadowDir = normalize(vec3f(normalize(toSun.xy + vec2f(1e-5)) * 0.88, 0.47));
    let shade = sunShadow(hit + vec3f(0.0, 0.0, 1.0), shadowDir, jitter) * (1.0 - haze);
    // Espuma en las crestas de las olas de proa.
    color = mix(color, vec3f(0.55, 0.60, 0.64), clamp(wake3.z, 0.0, 1.0) * 0.45 * (1.0 - haze));
    color = mix(color, foamColor, foam * 0.85 * (1.0 - 0.35 * shade)) * (1.0 - 0.3 * shade)
          + glint * (1.0 - 0.5 * haze) * (1.0 - shade);
    // Faroles de los barcos: el reflejo nace en el agua justo debajo del farol
    // (top) y se estira hacia su punto espejado (mir), como en el agua real con
    // ondas. Desde esta cámara cae debajo de cada barco. Tiembla y titila.
    let lampCount = i32(u.lightCount.x);
    for (var i = 0; i < 16; i++) {
        if (i >= lampCount) { break; }
        let lp = u.lights[i * 2].xyz;
        let lc = u.lights[i * 2 + 1].rgb;
        let top = toPixel(vec3f(lp.xy, u.params.x));
        let mir = toPixel(vec3f(lp.xy, 2.0 * u.params.x - lp.z));
        if (top.z < 0.0 || mir.z < 0.0) { continue; }
        // Luz compacta (el hada): sin columna, un charco de luz redondo en el
        // agua justo debajo, que tiembla con las olas. El punto espejado de
        // algo que vuela tan alto cae fuera del cuadro del login.
        if (u.lights[i * 2 + 1].w > 0.5) {
            if (overHull(lp, u.params.x) > 0.5) { continue; }
            let r = 0.010 * u.viewport.w;
            let d = (position.xy + g * 0.015 * u.viewport.w - top.xy) * vec2f(1.0, 2.2);
            let d2 = dot(d, d) / (r * r);
            let wa = 0.5 + 0.5 * sin(hit.y * 0.55 + g.y * 90.0 + u.params.y * 3.1);
            let wb = 0.5 + 0.5 * sin(hit.x * 0.80 + g.x * 70.0 - u.params.y * 2.2);
            let spot = exp(-d2) * (0.4 + 0.9 * pow(wa * wb, 1.5)) + exp(-d2 * 0.12) * 0.10;
            color += lc * spot * 0.6 * (1.0 - haze);
            continue;
        }
        let span = max(mir.y - top.y, 8.0);
        let along = (position.y - top.y) / span;
        let cx = mix(top.x, mir.x, clamp(along, 0.0, 1.0));
        let width = 0.004 * u.viewport.w * (0.8 + 1.5 * clamp(along, 0.0, 1.0));
        let dx = position.x + g.x * 0.015 * u.viewport.w - cx;
        let profile = smoothstep(0.0, 0.04, along) * (1.0 - smoothstep(0.25, 0.8, along));
        // La columna no es una línea lisa: son destellos sueltos donde la ola
        // inclina hacia la cámara, densos cerca del farol y ralos en la punta.
        let ph = f32(i) * 1.7;
        let wa = 0.5 + 0.5 * sin(hit.y * 0.55 + g.y * 90.0 + u.params.y * 3.1 + ph);
        let wb = 0.5 + 0.5 * sin(hit.x * 0.80 + g.x * 70.0 - u.params.y * 2.2 + ph * 2.3);
        let sparkle = pow(wa * wb, mix(1.5, 4.0, clamp(along, 0.0, 1.0)));
        let core = exp(-dx * dx / (width * width));
        let glow = exp(-dx * dx / (4.0 * width * width)) * 0.12;
        color += lc * (core * sparkle * 1.4 + glow) * profile * 0.55 * (1.0 - haze);
    }
    // Espuma en la línea de flotación: si cerca en pantalla hay un casco que
    // toca el agua (su punto está a poca distancia de este, a la altura del
    // agua), se aclara con un patrón que sigue a las olas.
    var nearHull = 1.0e9;
    for (var k = 0; k < 8; k++) {
        let a = f32(k) * 0.7853982;
        let o = vec2f(cos(a), sin(a)) * select(6.0, 14.0, k % 2 == 1);
        let sp = position.xy + o;
        let ds = textureLoad(depth, vec2i(sp), 0);
        if (ds < 1.0) {
            // Cuenta sólo si en ese píxel lo dibujado tapa al agua (el mar
            // pintado de abajo no) y está cerca de la línea de flotación.
            let ph = worldAtPixel(sp, ds);
            let dn = normalize(worldAtPixel(sp, 1.0) - cam);
            let tn = (u.params.x - cam.z) / min(dn.z, -0.0005);
            if (ph.z < u.params.x + 25.0 && length(ph - cam) < tn - 1.0) {
                nearHull = min(nearHull, length(ph.xy - hit.xy));
            }
        }
    }
    let ripple = 0.6 + 0.4 * sin(dot(hit.xy, vec2f(0.21, 0.17)) + u.params.y * 2.3 + g.x * 30.0);
    let hullFoam = (1.0 - smoothstep(6.0, 45.0, nearHull)) * ripple * (1.0 - haze);
    color = mix(color, vec3f(0.62, 0.66, 0.68), hullFoam * 0.7);
    return vec4f(color, 1.0);
}
)WGSL";
// ── Ondas del agua de los mapas ─────────────────────────────────────────────
// Ecuación de onda sobre una grilla de altura/velocidad (como el demo de
// WebGL/WebGPU Water): cada celda tiende al promedio de sus vecinas, con un
// poco de amortiguación. Lo que está en el agua desplaza agua según cuánto se
// hunde; sólo el CAMBIO empuja (moverse, salir, volver a caer), así un cuerpo
// quieto no cava un pozo. Donde no hay agua (G de la máscara en 0) la altura
// queda en 0 y la onda rebota.
constexpr int SimSize = 512;
constexpr float SimTexel = 4.0f;
const char* const WaveShader = R"WGSL(
struct Sim { origin: vec4f, info: vec4f, olds: array<vec4f, 8>, news: array<vec4f, 8> };
// Lo que desplaza un cuerpo en (x, y): hondo en el centro, nada en el radio.
fn displaced(w: vec2f, src: vec4f) -> f32 {
    let d = length(w - src.xy);
    if (src.w <= 0.0 || d >= src.w) { return 0.0; }
    return src.z * (0.5 + 0.5 * cos(d / src.w * 3.14159265));
}
@group(0) @binding(0) var prev: texture_2d<f32>;
@group(0) @binding(1) var next: texture_storage_2d<rgba32float, write>;
@group(0) @binding(2) var<uniform> s: Sim;
@group(0) @binding(3) var waterMask: texture_2d<f32>;
fn at(c: vec2i) -> vec2f {
    let n = i32(textureDimensions(prev).x);
    return textureLoad(prev, clamp(c, vec2i(0), vec2i(n - 1)), 0).xy;
}
@compute @workgroup_size(8, 8) fn simulate(@builtin(global_invocation_id) id: vec3u) {
    let n = i32(textureDimensions(prev).x);
    let c = vec2i(id.xy);
    if (c.x >= n || c.y >= n) { return; }
    if (s.origin.w > 0.5) { textureStore(next, c, vec4f(0.0)); return; }
    let w = s.origin.xy + (vec2f(c) + 0.5) * s.origin.z;
    let m = clamp(vec2i(floor(w / 100.0 + 0.5)), vec2i(0), vec2i(255));
    if (textureLoad(waterMask, m, 0).g <= 0.0) { textureStore(next, c, vec4f(0.0)); return; }
    var hv = at(c);
    let avg = (at(c + vec2i(1, 0)).x + at(c - vec2i(1, 0)).x + at(c + vec2i(0, 1)).x + at(c - vec2i(0, 1)).x) * 0.25;
    hv.y = (hv.y + (avg - hv.x) * 2.0) * 0.985;
    hv.x = (hv.x + hv.y) * 0.996;
    for (var i = 0; i < 8; i++) {
        if (f32(i) >= s.info.x) { break; }
        hv.x += displaced(w, s.olds[i]) - displaced(w, s.news[i]);
    }
    hv.x = clamp(hv.x, -20.0, 20.0);
    textureStore(next, c, vec4f(hv, 0.0, 0.0));
}
)WGSL";
bool EnsureWaves() {
    if (g.simPipeline) return true;
    for (int i = 0; i < 2; ++i) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = {uint32_t(SimSize), uint32_t(SimSize), 1};
        td.format = WGPUTextureFormat_RGBA32Float;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_StorageBinding;
        g.simTexture[i] = wgpuDeviceCreateTexture(GetDevice(), &td);
        if (!g.simTexture[i]) return false;
        g.simView[i] = wgpuTextureCreateView(g.simTexture[i], nullptr);
    }
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = WGPUStringView{WaveShader, strlen(WaveShader)};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    g.simModule = wgpuDeviceCreateShaderModule(GetDevice(), &md);
    WGPUBindGroupLayoutEntry e[4] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                     WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    e[0].binding = 0; e[0].visibility = WGPUShaderStage_Compute;
    e[0].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    e[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    e[1].binding = 1; e[1].visibility = WGPUShaderStage_Compute;
    e[1].storageTexture.access = WGPUStorageTextureAccess_WriteOnly;
    e[1].storageTexture.format = WGPUTextureFormat_RGBA32Float;
    e[1].storageTexture.viewDimension = WGPUTextureViewDimension_2D;
    e[2].binding = 2; e[2].visibility = WGPUShaderStage_Compute;
    e[2].buffer.type = WGPUBufferBindingType_Uniform;
    e[2].buffer.minBindingSize = 288;
    e[3].binding = 3; e[3].visibility = WGPUShaderStage_Compute;
    e[3].texture.sampleType = WGPUTextureSampleType_Float;
    e[3].texture.viewDimension = WGPUTextureViewDimension_2D;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 4; ld.entries = e;
    g.simLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &ld);
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 1; pd.bindGroupLayouts = &g.simLayout;
    g.simPipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
    WGPUComputePipelineDescriptor cd = WGPU_COMPUTE_PIPELINE_DESCRIPTOR_INIT;
    cd.layout = g.simPipelineLayout;
    cd.compute.module = g.simModule;
    cd.compute.entryPoint = View("simulate");
    g.simPipeline = wgpuDeviceCreateComputePipeline(GetDevice(), &cd);
    for (WGPUBuffer& b : g.simBuffer) b = CreateBuffer(288, WGPUBufferUsage_Uniform);
    return g.simPipeline != nullptr;
}
// Avanza la simulación a 60 pasos por segundo (hasta 4 por cuadro) y deja en
// g.water.sim el área que cubre.
void EncodeWaves(WGPUCommandEncoder encoder, float time) {
    g.water.sim[3] = 0.0f;
    if (!g.waterMaskView || !EnsureWaves()) return;
    // El área sigue al héroe en saltos de 512: al cambiar, empieza quieta.
    const float half = SimSize * SimTexel * 0.5f;
    const float ox = std::floor(g.simFocus[0] / 512.0f + 0.5f) * 512.0f - half;
    const float oy = std::floor(g.simFocus[1] / 512.0f + 0.5f) * 512.0f - half;
    bool clear = ox != g.simOrigin[0] || oy != g.simOrigin[1];
    g.simOrigin[0] = ox; g.simOrigin[1] = oy;
    int steps = 0;
    if (g.simTime < 0.0f || time < g.simTime || clear) { g.simTime = time; steps = 1; clear = true; }
    else { steps = int((time - g.simTime) * 60.0f); g.simTime += steps / 60.0f; }
    if (steps > 4) { steps = 4; g.simTime = time; }
    // Pares (antes, ahora) de lo que desplaza agua desde el último paso: cada
    // cuerpo se empareja con el más cercano de la vez anterior; el que aparece
    // no tenía nada antes y el que se fue ya no desplaza nada.
    float olds[8][4] = {}, news[8][4] = {};
    int pairs = 0;
    if (steps > 0) {
        bool used[8] = {};
        for (int i = 0; i < g.simSourceCount && pairs < 8; ++i) {
            const float* now = g.simSources[i];
            int best = -1; float bestD = 150.0f;
            for (int j = 0; j < g.simAppliedCount; ++j) {
                if (used[j]) continue;
                const float d = std::hypot(now[0] - g.simApplied[j][0], now[1] - g.simApplied[j][1]);
                if (d < bestD) { bestD = d; best = j; }
            }
            if (best >= 0) { used[best] = true; std::memcpy(olds[pairs], g.simApplied[best], sizeof(olds[0])); }
            std::memcpy(news[pairs++], now, sizeof(news[0]));
        }
        for (int j = 0; j < g.simAppliedCount && pairs < 8; ++j)
            if (!used[j]) std::memcpy(olds[pairs++], g.simApplied[j], sizeof(olds[0]));
        if (clear) pairs = 0;   // recién empezada: quieta, sin empujes
        std::memcpy(g.simApplied, g.simSources, sizeof(g.simApplied));
        g.simAppliedCount = g.simSourceCount;
    }
    for (int k = 0; k < steps; ++k) {
        float u[72] = {ox, oy, SimTexel, (clear && k == 0) ? 1.0f : 0.0f, k == 0 ? float(pairs) : 0.0f};
        std::memcpy(u + 8, olds, sizeof(olds));
        std::memcpy(u + 40, news, sizeof(news));
        wgpuQueueWriteBuffer(GetQueue(), g.simBuffer[k], 0, u, sizeof(u));
        WGPUBindGroupEntry be[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                    WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
        be[0].binding = 0; be[0].textureView = g.simView[g.simCurrent];
        be[1].binding = 1; be[1].textureView = g.simView[1 - g.simCurrent];
        be[2].binding = 2; be[2].buffer = g.simBuffer[k]; be[2].size = 288;
        be[3].binding = 3; be[3].textureView = g.waterMaskView;
        WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
        bd.layout = g.simLayout; bd.entryCount = 4; bd.entries = be;
        WGPUBindGroup group = wgpuDeviceCreateBindGroup(GetDevice(), &bd);
        WGPUComputePassEncoder pass = wgpuCommandEncoderBeginComputePass(encoder, nullptr);
        wgpuComputePassEncoderSetPipeline(pass, g.simPipeline);
        wgpuComputePassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
        wgpuComputePassEncoderDispatchWorkgroups(pass, SimSize / 8, SimSize / 8, 1);
        wgpuComputePassEncoderEnd(pass);
        wgpuComputePassEncoderRelease(pass);
        g.retiredGroups.push_back(group);
        g.simCurrent = 1 - g.simCurrent;
    }
    g.water.sim[0] = ox; g.water.sim[1] = oy; g.water.sim[2] = SimTexel; g.water.sim[3] = 1.0f;
}

bool EnsureWater() {
    const WGPUTextureFormat format = SurfaceFormat();
    const uint32_t samples = Samples();
    if (g.waterPipeline && g.waterFormat == format && g.waterSamples == samples) return true;
    if (g.waterPipeline) wgpuRenderPipelineRelease(g.waterPipeline);
    if (g.waterPipelineLayout) wgpuPipelineLayoutRelease(g.waterPipelineLayout);
    if (g.waterLayout) wgpuBindGroupLayoutRelease(g.waterLayout);
    if (g.waterModule) wgpuShaderModuleRelease(g.waterModule);
    g.waterPipeline = nullptr;
    std::string code = WaterShader;
    const char* type = samples > 1 ? "texture_depth_multisampled_2d" : "texture_depth_2d";
    code.replace(code.find("DEPTH_TYPE"), 10, type);
    WGPUShaderSourceWGSL wgsl = WGPU_SHADER_SOURCE_WGSL_INIT;
    wgsl.code = WGPUStringView{code.c_str(), code.size()};
    WGPUShaderModuleDescriptor md = WGPU_SHADER_MODULE_DESCRIPTOR_INIT;
    md.nextInChain = &wgsl.chain;
    g.waterModule = wgpuDeviceCreateShaderModule(GetDevice(), &md);
    WGPUBindGroupLayoutEntry entries[6] = {WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT,
                                           WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT, WGPU_BIND_GROUP_LAYOUT_ENTRY_INIT};
    entries[5].binding = 5;
    entries[5].visibility = WGPUShaderStage_Fragment;
    entries[5].texture.sampleType = WGPUTextureSampleType_UnfilterableFloat;
    entries[5].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[0].binding = 0;
    entries[0].visibility = WGPUShaderStage_Fragment;
    entries[0].texture.sampleType = WGPUTextureSampleType_Depth;
    entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[0].texture.multisampled = samples > 1;
    entries[1].binding = 1;
    entries[1].visibility = WGPUShaderStage_Fragment;
    entries[1].buffer.type = WGPUBufferBindingType_Uniform;
    entries[1].buffer.minBindingSize = sizeof(State::Water);
    entries[4].binding = 4;
    entries[4].visibility = WGPUShaderStage_Fragment;
    entries[4].texture.sampleType = WGPUTextureSampleType_Float;
    entries[4].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[2].binding = 2;
    entries[2].visibility = WGPUShaderStage_Fragment;
    entries[2].texture.sampleType = WGPUTextureSampleType_Float;
    entries[2].texture.viewDimension = WGPUTextureViewDimension_2D;
    entries[3].binding = 3;
    entries[3].visibility = WGPUShaderStage_Fragment;
    entries[3].sampler.type = WGPUSamplerBindingType_Filtering;
    WGPUBindGroupLayoutDescriptor ld = WGPU_BIND_GROUP_LAYOUT_DESCRIPTOR_INIT;
    ld.entryCount = 6; ld.entries = entries;
    if (!g.waterSampler) {
        WGPUSamplerDescriptor sd = WGPU_SAMPLER_DESCRIPTOR_INIT;
        sd.magFilter = WGPUFilterMode_Linear;
        sd.minFilter = WGPUFilterMode_Linear;
        g.waterSampler = wgpuDeviceCreateSampler(GetDevice(), &sd);
    }
    g.waterLayout = wgpuDeviceCreateBindGroupLayout(GetDevice(), &ld);
    WGPUPipelineLayoutDescriptor pd = WGPU_PIPELINE_LAYOUT_DESCRIPTOR_INIT;
    pd.bindGroupLayoutCount = 1; pd.bindGroupLayouts = &g.waterLayout;
    g.waterPipelineLayout = wgpuDeviceCreatePipelineLayout(GetDevice(), &pd);
    if (!g.waterBuffer) g.waterBuffer = CreateBuffer(sizeof(State::Water), WGPUBufferUsage_Uniform);
    WGPUBlendState blend{};
    blend.color = {WGPUBlendOperation_Add, WGPUBlendFactor_SrcAlpha, WGPUBlendFactor_OneMinusSrcAlpha};
    blend.alpha = {WGPUBlendOperation_Add, WGPUBlendFactor_Zero, WGPUBlendFactor_One};
    WGPUColorTargetState target = WGPU_COLOR_TARGET_STATE_INIT;
    target.format = format;
    target.blend = &blend;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = WGPU_FRAGMENT_STATE_INIT;
    fragment.module = g.waterModule;
    fragment.entryPoint = View("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor d = WGPU_RENDER_PIPELINE_DESCRIPTOR_INIT;
    d.layout = g.waterPipelineLayout;
    d.multisample.count = samples;
    d.vertex.module = g.waterModule;
    d.vertex.entryPoint = View("vs");
    d.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    d.fragment = &fragment;
    g.waterPipeline = wgpuDeviceCreateRenderPipeline(GetDevice(), &d);
    g.waterFormat = format; g.waterSamples = samples;
    return g.waterPipeline && g.waterBuffer;
}
// Máscara de agua del mapa (256x256 RGBA; ver WaterProfiles). nullptr deja
// una de 1x1 vacía, para el login y antes de cargar un mapa.
void UploadWaterMask(const unsigned char* mask) {
    const int side = mask ? 256 : 1;
    if (g.waterMask && int(wgpuTextureGetWidth(g.waterMask)) != side) {
        wgpuTextureViewRelease(g.waterMaskView); wgpuTextureRelease(g.waterMask);
        g.waterMaskView = nullptr; g.waterMask = nullptr;
    }
    if (!g.waterMask) {
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = {uint32_t(side), uint32_t(side), 1};
        td.format = WGPUTextureFormat_RGBA8Unorm;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        g.waterMask = wgpuDeviceCreateTexture(GetDevice(), &td);
        if (!g.waterMask) return;
        g.waterMaskView = wgpuTextureCreateView(g.waterMask, nullptr);
    }
    std::vector<uint8_t> rgba(size_t(side) * side * 4, 0);
    if (mask) std::memcpy(rgba.data(), mask, rgba.size());
    Write(g.waterMask, 0, 0, side, side, rgba);
}
void EncodeWater(WGPUCommandEncoder encoder, unsigned width, unsigned height) {
    if (!g.depthView || !EnsureWater()) return;
    const int* v = g.waterViewport;
    const float x = float(std::max(0, v[0]));
    const float w = std::min(float(v[2]), float(width) - x);
    const float top = float(int(height) - v[1] - v[3]);
    const float y = std::max(0.0f, top);
    const float h = std::min(float(v[3]) - (y - top), float(height) - y);
    if (w <= 0 || h <= 0) return;
    if (!g.waterMaskView) UploadWaterMask(nullptr);
    // Ondas: sólo con el agua de los mapas.
    if (g.water.params[3] > 0.5f) EncodeWaves(encoder, g.water.params[1]);
    else g.water.sim[3] = 0.0f;
    if (!EnsureWaves()) return;
    g.water.viewport[0] = float(v[0]); g.water.viewport[1] = top;
    g.water.viewport[2] = float(v[2]); g.water.viewport[3] = float(v[3]);
    wgpuQueueWriteBuffer(GetQueue(), g.waterBuffer, 0, &g.water, sizeof(g.water));
    // Copia de lo dibujado hasta ahora (ya resuelto si hay MSAA): los rayos del
    // reflejo la leen, y el agua se dibuja encima del original.
    const WGPUTextureFormat format = SurfaceFormat();
    if (!g.waterScene || g.waterSceneWidth != width || g.waterSceneHeight != height || g.waterSceneFormat != format) {
        if (g.waterSceneView) wgpuTextureViewRelease(g.waterSceneView);
        if (g.waterScene) wgpuTextureRelease(g.waterScene);
        WGPUTextureDescriptor td = WGPU_TEXTURE_DESCRIPTOR_INIT;
        td.size = {width, height, 1};
        td.format = format;
        td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
        g.waterScene = wgpuDeviceCreateTexture(GetDevice(), &td);
        g.waterSceneView = wgpuTextureCreateView(g.waterScene, nullptr);
        g.waterSceneWidth = width; g.waterSceneHeight = height; g.waterSceneFormat = format;
    }
    if (!g.waterSceneView) return;
    // Sin máscara (el login, o antes de cargar un mapa) va una de 1x1 vacía.
    if (!g.waterMaskView) UploadWaterMask(nullptr);
    if (!g.waterMaskView) return;
    {
        WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT, to = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
        from.texture = FrameTexture(); to.texture = g.waterScene;
        const WGPUExtent3D size{width, height, 1};
        wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &size);
    }
    WGPUBindGroupEntry entries[6] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                     WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
    entries[0].binding = 0; entries[0].textureView = g.depthView;
    entries[1].binding = 1; entries[1].buffer = g.waterBuffer; entries[1].size = sizeof(State::Water);
    entries[2].binding = 2; entries[2].textureView = g.waterSceneView;
    entries[3].binding = 3; entries[3].sampler = g.waterSampler;
    entries[4].binding = 4; entries[4].textureView = g.waterMaskView;
    entries[5].binding = 5; entries[5].textureView = g.simView[g.simCurrent];
    WGPUBindGroupDescriptor bd = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
    bd.layout = g.waterLayout; bd.entryCount = 6; bd.entries = entries;
    WGPUBindGroup group = wgpuDeviceCreateBindGroup(GetDevice(), &bd);
    WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
    ColorTarget(color);
    color.loadOp = WGPULoadOp_Load;
    color.storeOp = WGPUStoreOp_Store;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &color;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderSetViewport(pass, x, y, w, h, 0, 1);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, group, 0, nullptr);
    wgpuRenderPassEncoderSetPipeline(pass, g.waterPipeline);
    wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
    g.retiredGroups.push_back(group);
}

// Pase de sombras: los dibujos marcados como proyectores, vistos desde la luz.
void EncodeShadows(WGPUCommandEncoder encoder) {
    WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
    depth.view = g.shadowView;
    depth.depthLoadOp = WGPULoadOp_Clear;
    depth.depthStoreOp = WGPUStoreOp_Store;
    depth.depthClearValue = 1.0f;
    WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
    rp.colorAttachmentCount = 0;
    rp.depthStencilAttachment = &depth;
    WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
    wgpuRenderPassEncoderSetBindGroup(pass, 2, g.casterGroup, 0, nullptr);
    wgpuRenderPassEncoderSetBindGroup(pass, 0, g.uniformGroup, 0, nullptr);
    WGPURenderPipeline lastPipeline = nullptr;
    WGPUBindGroup lastTexture = nullptr;
    WGPUBuffer lastVertices = nullptr, lastIndices = nullptr;
    for (const auto& recorded : g.passes) {
        for (const auto& draw : recorded.draws) {
            if (!draw.caster) continue;
            const uint8_t topology = draw.vertexBuffer ? uint8_t(WGPUPrimitiveTopology_TriangleList | (Resident << 4))
                                   : uint8_t(draw.quads ? WGPUPrimitiveTopology_TriangleList : draw.topology);
            const WGPURenderPipeline pipeline = CasterPipeline(topology);
            if (pipeline != lastPipeline) { wgpuRenderPassEncoderSetPipeline(pass, pipeline); lastPipeline = pipeline; }
            const WGPUBuffer vertices = draw.vertexBuffer ? draw.vertexBuffer : g.vertexBuffer;
            if (vertices != lastVertices) { wgpuRenderPassEncoderSetVertexBuffer(pass, 0, vertices, 0, WGPU_WHOLE_SIZE); lastVertices = vertices; }
            const WGPUBuffer indices = draw.vertexBuffer ? draw.indexBuffer : draw.quads ? g.indexBuffer : nullptr;
            if (indices && indices != lastIndices) {
                wgpuRenderPassEncoderSetIndexBuffer(pass, indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
                lastIndices = indices;
            }
            if (draw.textureGroup != lastTexture) { wgpuRenderPassEncoderSetBindGroup(pass, 1, draw.textureGroup, 0, nullptr); lastTexture = draw.textureGroup; }
            if (draw.vertexBuffer) wgpuRenderPassEncoderDrawIndexed(pass, draw.indexCount, 1, 0, 0, draw.id);
            else if (draw.quads) wgpuRenderPassEncoderDrawIndexed(pass, draw.vertexCount / 4 * 6, 1, 0, int32_t(draw.firstVertex), draw.id);
            else wgpuRenderPassEncoderDraw(pass, draw.vertexCount, 1, draw.firstVertex, draw.id);
        }
    }
    wgpuRenderPassEncoderEnd(pass);
    wgpuRenderPassEncoderRelease(pass);
}

// Toma un staging mapeado de al menos `size` bytes; si no hay libre, crea uno
// mapeado de entrada. Los que están en uso vuelven al terminar su remapeo.
Staging* AcquireStaging(uint64_t size) {
    Staging* best = nullptr;
    for (auto& staging : g.staging)
        if (staging->mapped && staging->size >= size && (!best || staging->size < best->size)) best = staging.get();
    if (best) return best;
    // Si hay uno libre pero chico, se reemplaza por uno más grande.
    for (auto it = g.staging.begin(); it != g.staging.end(); ++it)
        if ((*it)->mapped) {
            wgpuBufferUnmap((*it)->buffer);
            wgpuBufferRelease((*it)->buffer);
            g.staging.erase(it);
            break;
        }
    uint64_t capacity = 1u << 20;
    while (capacity < size) capacity *= 2;
    WGPUBufferDescriptor d = WGPU_BUFFER_DESCRIPTOR_INIT;
    d.size = capacity;
    d.usage = WGPUBufferUsage_MapWrite | WGPUBufferUsage_CopySrc;
    d.mappedAtCreation = true;
    auto staging = std::make_unique<Staging>();
    staging->buffer = wgpuDeviceCreateBuffer(GetDevice(), &d);
    if (!staging->buffer) return nullptr;
    staging->size = capacity;
    staging->mapped = static_cast<uint8_t*>(wgpuBufferGetMappedRange(staging->buffer, 0, size_t(capacity)));
    if (!staging->mapped) { wgpuBufferRelease(staging->buffer); return nullptr; }
    g.staging.push_back(std::move(staging));
    return g.staging.back().get();
}
// Tras el envío se vuelve a mapear: el callback llega en wgpuInstanceProcessEvents
// (lo llama Present) cuando la GPU ya terminó la copia.
void RemapStaging(Staging& staging) {
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* user, void*) {
        auto* staging = static_cast<Staging*>(user);
        if (staging->orphan) { delete staging; return; }
        if (status != WGPUMapAsyncStatus_Success) {
            // No se pudo mapear: se descarta y la próxima subida crea otro.
            for (auto it = g.staging.begin(); it != g.staging.end(); ++it)
                if (it->get() == staging) { wgpuBufferRelease(staging->buffer); g.staging.erase(it); break; }
            return;
        }
        staging->mapped = static_cast<uint8_t*>(wgpuBufferGetMappedRange(staging->buffer, 0, size_t(staging->size)));
    };
    callback.userdata1 = &staging;
    wgpuBufferMapAsync(staging.buffer, WGPUMapMode_Write, 0, size_t(staging.size), callback);
}

// Codifica y envía lo grabado hasta ahora sobre la textura del cuadro. La
// grabación sigue después sobre la misma textura (lo usa la captura).
void EncodePasses() {
    if (g.passes.empty() || !BeginFrame()) { ResetFrame(); ReleaseRetired(); return; }
    const unsigned width = Width(), height = Height();
    EnsureDepth(width, height);
    EnsureMsaa(width, height);
    struct Upload { WGPUBuffer buffer; const void* data; uint64_t size; };
    Upload uploads[5]{};
    int uploadCount = 0;
    if (!g.vertices.empty()) {
        Reserve(g.vertexBuffer, g.vertexCapacity, g.vertices.size() * sizeof(Vertex), WGPUBufferUsage_Vertex);
        uploads[uploadCount++] = {g.vertexBuffer, g.vertices.data(), g.vertices.size() * sizeof(Vertex)};
    }
    if (!g.uniforms.empty()) {
        // El grupo 0 siempre lleva skin y paleta, aunque el cuadro no tenga mallas.
        if (g.skins.empty()) g.skins.resize(UniformStride);
        if (g.palette.empty()) g.palette.resize(4);
        Reserve(g.uniformBuffer, g.uniformCapacity, g.uniforms.size(), WGPUBufferUsage_Storage);
        Reserve(g.skinBuffer, g.skinCapacity, g.skins.size(), WGPUBufferUsage_Storage);
        // Tabla de dibujos: el índice de cada dibujo va en firstInstance.
        g.drawTable.clear();
        for (auto& pass : g.passes)
            for (auto& draw : pass.draws) {
                draw.id = uint32_t(g.drawTable.size() / 2);
                g.drawTable.push_back(draw.uniformOffset / uint32_t(UniformStride));
                g.drawTable.push_back(draw.skinOffset / uint32_t(UniformStride));
            }
        if (g.drawTable.empty()) g.drawTable.assign(2, 0);
        Reserve(g.drawBuffer, g.drawCapacity, g.drawTable.size() * sizeof(uint32_t), WGPUBufferUsage_Storage);
        Reserve(g.paletteBuffer, g.paletteCapacity, g.palette.size() * sizeof(float), WGPUBufferUsage_Storage);
        uploads[uploadCount++] = {g.uniformBuffer, g.uniforms.data(), g.uniforms.size()};
        uploads[uploadCount++] = {g.skinBuffer, g.skins.data(), g.skins.size()};
        uploads[uploadCount++] = {g.paletteBuffer, g.palette.data(), g.palette.size() * sizeof(float)};
        uploads[uploadCount++] = {g.drawBuffer, g.drawTable.data(), g.drawTable.size() * sizeof(uint32_t)};
        if (!g.uniformGroup) {
            WGPUBindGroupEntry entries[4] = {WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT,
                                             WGPU_BIND_GROUP_ENTRY_INIT, WGPU_BIND_GROUP_ENTRY_INIT};
            entries[0].binding = 0; entries[0].buffer = g.uniformBuffer; entries[0].size = g.uniformCapacity;
            entries[1].binding = 1; entries[1].buffer = g.skinBuffer; entries[1].size = g.skinCapacity;
            entries[2].binding = 2; entries[2].buffer = g.paletteBuffer; entries[2].size = g.paletteCapacity;
            entries[3].binding = 3; entries[3].buffer = g.drawBuffer; entries[3].size = g.drawCapacity;
            WGPUBindGroupDescriptor d = WGPU_BIND_GROUP_DESCRIPTOR_INIT;
            d.layout = g.uniformLayout; d.entryCount = 4; d.entries = entries;
            g.uniformGroup = wgpuDeviceCreateBindGroup(GetDevice(), &d);
        }
    }
    uint64_t quads = 0;
    for (const auto& pass : g.passes)
        for (const auto& draw : pass.draws) if (draw.quads) quads = std::max<uint64_t>(quads, draw.vertexCount / 4);
    if (quads) ReserveQuadIndices(quads);

    // Sombras: el uniform va siempre (el grupo 2 está en todos los pipelines);
    // sin escena en el cuadro, los receptores no leen el mapa.
    g.shadow.params[0] = g.shadowFrame ? 1.0f : 0.0f;
    wgpuQueueWriteBuffer(GetQueue(), g.shadowBuffer, 0, &g.shadow, sizeof(g.shadow));
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(GetDevice(), nullptr);
    // Las subidas van antes de las pasadas: la copia queda ordenada en el encoder.
    // Todos los tamaños son múltiplos de 4 (vértices, filas de 256, floats, u32).
    uint64_t uploadBytes = 0;
    for (int i = 0; i < uploadCount; ++i) uploadBytes += (uploads[i].size + 3) & ~uint64_t(3);
    Staging* staging = uploadBytes ? AcquireStaging(uploadBytes) : nullptr;
    uint64_t at = 0;
    for (int i = 0; i < uploadCount; ++i) {
        if (!staging) { wgpuQueueWriteBuffer(GetQueue(), uploads[i].buffer, 0, uploads[i].data, uploads[i].size); continue; }
        std::memcpy(staging->mapped + at, uploads[i].data, size_t(uploads[i].size));
        wgpuCommandEncoderCopyBufferToBuffer(encoder, staging->buffer, at, uploads[i].buffer, 0, uploads[i].size);
        at += (uploads[i].size + 3) & ~uint64_t(3);
    }
    if (staging) { staging->mapped = nullptr; wgpuBufferUnmap(staging->buffer); }
    if (g.shadowFrame && g.uniformGroup) EncodeShadows(encoder);
    for (const auto& pass : g.passes) {
        if (pass.atmosphere) EncodeAtmosphere(encoder, width, height);
        if (pass.water) EncodeWater(encoder, width, height);
        if (pass.Post()) EncodePost(encoder, pass, width, height);
        WGPURenderPassColorAttachment color = WGPU_RENDER_PASS_COLOR_ATTACHMENT_INIT;
        ColorTarget(color);
        color.loadOp = pass.clearColor ? WGPULoadOp_Clear : WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {pass.color[0], pass.color[1], pass.color[2], pass.color[3]};
        WGPURenderPassDepthStencilAttachment depth = WGPU_RENDER_PASS_DEPTH_STENCIL_ATTACHMENT_INIT;
        depth.view = g.depthView;
        depth.depthLoadOp = pass.clearDepth ? WGPULoadOp_Clear : WGPULoadOp_Load;
        depth.depthStoreOp = WGPUStoreOp_Store;
        depth.depthClearValue = 1.0f;
        WGPURenderPassDescriptor rp = WGPU_RENDER_PASS_DESCRIPTOR_INIT;
        rp.colorAttachmentCount = 1;
        rp.colorAttachments = &color;
        rp.depthStencilAttachment = &depth;
        WGPURenderPassEncoder encoderPass = wgpuCommandEncoderBeginRenderPass(encoder, &rp);
        wgpuRenderPassEncoderSetBindGroup(encoderPass, 2, g.shadowGroup, 0, nullptr);
        if (g.uniformGroup) wgpuRenderPassEncoderSetBindGroup(encoderPass, 0, g.uniformGroup, 0, nullptr);
        // Dawn valida cada comando: sólo se emite el estado que cambia.
        WGPURenderPipeline lastPipeline = nullptr;
        WGPUBindGroup lastTexture = nullptr;
        WGPUBuffer lastVertices = nullptr, lastIndices = nullptr;
        int lastViewport[4]{-1, -1, -1, -1};
        for (const auto& draw : pass.draws) {
            if (std::memcmp(lastViewport, draw.viewport, sizeof(lastViewport))) {
                // Viewport de OpenGL (origen abajo) a WebGPU (origen arriba), dentro del cuadro.
                const float x = float(std::max(0, draw.viewport[0]));
                const float w = std::min(float(draw.viewport[2]), float(width) - x);
                const float top = float(int(height) - draw.viewport[1] - draw.viewport[3]);
                const float y = std::max(0.0f, top);
                const float h = std::min(float(draw.viewport[3]) - (y - top), float(height) - y);
                if (w <= 0 || h <= 0) continue;
                wgpuRenderPassEncoderSetViewport(encoderPass, x, y, w, h, 0, 1);
                std::memcpy(lastViewport, draw.viewport, sizeof(lastViewport));
            }
            if (draw.pipeline != lastPipeline) { wgpuRenderPassEncoderSetPipeline(encoderPass, draw.pipeline); lastPipeline = draw.pipeline; }
            const WGPUBuffer vertices = draw.vertexBuffer ? draw.vertexBuffer : g.vertexBuffer;
            if (vertices != lastVertices) {
                wgpuRenderPassEncoderSetVertexBuffer(encoderPass, 0, vertices, 0, WGPU_WHOLE_SIZE);
                lastVertices = vertices;
            }
            const WGPUBuffer indices = draw.vertexBuffer ? draw.indexBuffer : draw.quads ? g.indexBuffer : nullptr;
            if (indices && indices != lastIndices) {
                wgpuRenderPassEncoderSetIndexBuffer(encoderPass, indices, WGPUIndexFormat_Uint32, 0, WGPU_WHOLE_SIZE);
                lastIndices = indices;
            }
            if (draw.textureGroup != lastTexture) {
                wgpuRenderPassEncoderSetBindGroup(encoderPass, 1, draw.textureGroup, 0, nullptr);
                lastTexture = draw.textureGroup;
            }
            if (draw.vertexBuffer) wgpuRenderPassEncoderDrawIndexed(encoderPass, draw.indexCount, 1, 0, 0, draw.id);
            else if (draw.quads) wgpuRenderPassEncoderDrawIndexed(encoderPass, draw.vertexCount / 4 * 6, 1, 0, int32_t(draw.firstVertex), draw.id);
            else wgpuRenderPassEncoderDraw(encoderPass, draw.vertexCount, 1, draw.firstVertex, draw.id);
        }
        wgpuRenderPassEncoderEnd(encoderPass);
        wgpuRenderPassEncoderRelease(encoderPass);
    }
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(GetQueue(), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    if (staging) RemapStaging(*staging);
    ResetFrame();
    ReleaseRetired();
}
}

// Envía el cuadro grabado y presenta. Lo llama CWindow::Present.
void SubmitFrame() {
    if (!g.ready) return;
    EncodePasses();
    if (FrameView()) Present();
}

// glReadPixels sobre el cuadro en curso: envía lo grabado, copia la región a
// un buffer y la lee esperando a la GPU. Filas de abajo hacia arriba, en RGB.
void ReadPixels(int x, int y, int width, int height, void* out) {
    auto* dst = static_cast<uint8_t*>(out);
    std::memset(dst, 0, size_t(width) * size_t(height) * 3);
    if (!g.ready) return;
    EncodePasses();
    if (!BeginFrame()) return;
    const int surfaceW = int(Width()), surfaceH = int(Height());
    const int x0 = std::max(0, x), x1 = std::min(surfaceW, x + width);
    const int y0 = std::max(0, y), y1 = std::min(surfaceH, y + height);
    if (x1 <= x0 || y1 <= y0) return;
    const uint32_t w = uint32_t(x1 - x0), h = uint32_t(y1 - y0);
    const uint32_t bytesPerRow = (w * 4 + 255) & ~255u;
    WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
    bd.size = uint64_t(bytesPerRow) * h;
    bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
    WGPUBuffer readback = wgpuDeviceCreateBuffer(GetDevice(), &bd);
    WGPUTexelCopyTextureInfo from = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
    from.texture = FrameTexture();
    from.origin = {uint32_t(x0), uint32_t(surfaceH - y1), 0};   // origen de OpenGL abajo
    WGPUTexelCopyBufferInfo to = WGPU_TEXEL_COPY_BUFFER_INFO_INIT;
    to.buffer = readback;
    to.layout.bytesPerRow = bytesPerRow;
    to.layout.rowsPerImage = h;
    const WGPUExtent3D size{w, h, 1};
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(GetDevice(), nullptr);
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &from, &to, &size);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(GetQueue(), 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    struct Result { bool ok = false; } result;
    WGPUBufferMapCallbackInfo callback = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
    callback.mode = WGPUCallbackMode_WaitAnyOnly;
    callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView, void* user, void*) {
        static_cast<Result*>(user)->ok = status == WGPUMapAsyncStatus_Success;
    };
    callback.userdata1 = &result;
    if (WaitFuture(wgpuBufferMapAsync(readback, WGPUMapMode_Read, 0, size_t(bd.size), callback)) && result.ok) {
        const auto* src = static_cast<const uint8_t*>(wgpuBufferGetConstMappedRange(readback, 0, size_t(bd.size)));
        const bool bgra = SurfaceFormat() == WGPUTextureFormat_BGRA8Unorm;
        // Fila r del resultado (desde abajo) = fila h-1-r de la copia (desde arriba).
        for (uint32_t r = 0; src && r < h; ++r) {
            const uint8_t* row = src + size_t(h - 1 - r) * bytesPerRow;
            uint8_t* line = dst + (size_t(y0 - y + int(r)) * size_t(width) + size_t(x0 - x)) * 3;
            for (uint32_t c = 0; c < w; ++c) {
                line[c * 3 + 0] = row[c * 4 + (bgra ? 2 : 0)];
                line[c * 3 + 1] = row[c * 4 + 1];
                line[c * 3 + 2] = row[c * 4 + (bgra ? 0 : 2)];
            }
        }
        wgpuBufferUnmap(readback);
    }
    wgpuBufferRelease(readback);
}

} // namespace WebGPU

// ── GfxDevice ───────────────────────────────────────────────────────────────
namespace GfxDevice::WGPU {
using namespace Gfx;
using ::WebGPU::g;
void Reset() {
    // Como el viewport inicial de OpenGL: la superficie entera.
    auto& v = RenderState::Mutable().viewport;
    v[0] = 0; v[1] = 0; v[2] = int(::WebGPU::Width()); v[3] = int(::WebGPU::Height());
}
// El estado se lee de RenderState al grabar cada lote.
void ApplyCap(Cap, bool) {}
void ApplyBlend(Factor, Factor) {}
void ApplyDepthFunc(Compare) {}
void ApplyDepthMask(bool) {}
void ApplyTexture(Texture) {}
void ApplyViewport(int, int, int, int) {}
void ClearColor(float r, float g_, float b, float a) {
    g.clearColor[0] = r; g.clearColor[1] = g_; g.clearColor[2] = b; g.clearColor[3] = a;
}
void Clear(unsigned clearMask) {
    if (!g.ready) return;
    // Un Clear antes de dibujar se suma a la pasada; si no, abre otra.
    if (g.passes.empty() || !g.passes.back().draws.empty() || g.passes.back().Post()) g.passes.emplace_back();
    auto& pass = g.passes.back();
    if (clearMask & ClearColorBuffer) {
        pass.clearColor = true;
        std::memcpy(pass.color, g.clearColor, sizeof(pass.color));
    }
    if (clearMask & ClearDepthBuffer) pass.clearDepth = true;
}
void Flush() {}
void ClearErrors() {}
bool SupportsShadowMaps() { return g.ready; }
// Abre o cierra la escena de sombras del cuadro. Al abrirla, la vista activa
// es la de la cámara: con su inversa cada dibujo recupera su posición en el
// mundo, cualquiera sea su modelView.
void ShadowScene(bool begin, const float* focus, float strength) {
    if (!g.ready) return;
    g.matrixRevision = ~0ull;   // los uniforms siguientes cambian el flag de receptor
    if (!begin) {
        // Niebla y cielo: una pasada nueva que se aplica antes de lo que siga.
        if (g.shadowScene && (g.atmosphere.params[0] > 0.5f || g.atmosphere.sky0[3] > 0.5f)) {
            g.passes.emplace_back();
            g.passes.back().atmosphere = true;
        }
        g.shadowScene = false;
        return;
    }
    g.shadowScene = focus != nullptr;
    g.shadowCasting = true;
    g.atmosphere.params[0] = 0.0f;
    g.atmosphere.sky0[3] = 0.0f;
    // Sin Lighting() en el cuadro, la luz queda como la del 0.97k.
    g.shadow.lighting[0] = 0.0f; g.shadow.lighting[1] = 0.0f; g.shadow.lighting[2] = 1.0f;
    g.shadow.fx[0] = 0.0f;
    if (!g.shadowScene) return;
    if (!ShadowLight::Invert(RenderState::Top(Gfx::Matrix::ModelView), g.shadow.inverseView)) { g.shadowScene = false; return; }
    // Para la niebla: inversa de proyección x vista, cámara y viewport.
    float viewProj[16];
    ShadowLight::Multiply(RenderState::Top(Gfx::Matrix::Projection), RenderState::Top(Gfx::Matrix::ModelView), viewProj);
    if (!ShadowLight::Invert(viewProj, g.atmosphere.inverseViewProj)) { g.shadowScene = false; return; }
    std::memcpy(g.atmosphere.camera, &g.shadow.inverseView[12], 3 * sizeof(float));
    std::memcpy(g.atmosphereViewport, RenderState::Get().viewport, sizeof(g.atmosphereViewport));
    if (strength <= 0.0f) return;   // escena sólo para la niebla
    ShadowLight::ViewProj(focus, g.shadow.lightViewProj);
    g.shadow.params[1] = strength;
    g.shadow.params[2] = 1.0f / float(::WebGPU::ShadowSize);
    g.shadow.params[3] = ShadowLight::Bias;
    g.shadowFrame = true;
}
void Atmosphere(int mode, const float* color, float density, float start) {
    if (!g.ready || !g.shadowScene) return;
    g.atmosphere.params[0] = float(mode);
    g.atmosphere.params[1] = density;
    g.atmosphere.params[2] = start;
    if (color) std::memcpy(g.atmosphere.color, color, 3 * sizeof(float));
}
// Mar (prueba): toma la cámara activa y abre una pasada que lo dibuja antes de
// lo que siga. params = altura, tiempo, sol (x, y, z).
void Water(const float* params) {
    if (!g.ready || !params) return;
    float inverseView[16], viewProj[16];
    if (!ShadowLight::Invert(RenderState::Top(Gfx::Matrix::ModelView), inverseView)) return;
    ShadowLight::Multiply(RenderState::Top(Gfx::Matrix::Projection), RenderState::Top(Gfx::Matrix::ModelView), viewProj);
    if (!ShadowLight::Invert(viewProj, g.water.inverseViewProj)) return;
    std::memcpy(g.water.viewProj, viewProj, sizeof(viewProj));
    std::memcpy(g.water.camera, &inverseView[12], 3 * sizeof(float));
    g.water.params[0] = params[0];
    g.water.params[1] = params[1];
    g.water.params[2] = params[5];
    std::memcpy(g.water.sun, params + 2, 3 * sizeof(float));
    g.water.lightCount[0] = params[8];
    std::memcpy(g.water.lights, params + 12, sizeof(g.water.lights));
    g.water.lightCount[1] = params[9];
    std::memcpy(g.water.ships, params + 12 + 128, sizeof(g.water.ships));
    g.water.params[3] = params[6];
    std::memcpy(g.water.tint, params + 12 + 128 + 16, 8 * sizeof(float));
    g.simFocus[0] = params[12 + 128 + 16 + 8];
    g.simFocus[1] = params[12 + 128 + 16 + 9];
    // Lo que está en el agua (luces con w = 2): hondura en color.r, radio en color.g.
    g.simSourceCount = 0;
    for (int i = 0; i < 16 && i < int(params[8]) && g.simSourceCount < 8; ++i) {
        const float* l = params + 12 + i * 8;
        if (l[7] < 1.5f) continue;
        float* src = g.simSources[g.simSourceCount++];
        src[0] = l[0]; src[1] = l[1]; src[2] = l[4]; src[3] = l[5];
    }
    std::memcpy(g.waterViewport, RenderState::Get().viewport, sizeof(g.waterViewport));
    g.passes.emplace_back();
    g.passes.back().water = true;
    // Agua redibujada (a ras del suelo): lo aditivo de los objetos del mapa,
    // como los arcos de Noria, va después del agua. No escribe profundidad y
    // el agua lo pisaba como si fuera el suelo de atrás.
    if (g.water.tint[3] <= 0.5f || g.passes.size() < 2) return;
    auto& before = g.passes[g.passes.size() - 2].draws;
    auto& after = g.passes.back().draws;
    size_t kept = 0;
    for (const auto& draw : before) {
        if (draw.overWater) after.push_back(draw);
        else before[kept++] = draw;
    }
    before.resize(kept);
}
void WaterMask(const unsigned char* mask) { if (g.ready) ::WebGPU::UploadWaterMask(mask); }
// Ver Gfx::Lighting: [0] luces, [1] intensidad, [2] luz base, [4] cáusticas,
// [5] tiempo, [8] sombra del sol, [9] suavidad; desde [12], 8 floats por luz.
void Lighting(const float* params) {
    if (!g.ready || !g.shadowScene) return;
    const int count = std::clamp(int(params[0]), 0, 64);
    g.shadow.lighting[0] = float(count);
    g.shadow.lighting[1] = params[1];
    g.shadow.lighting[2] = params[2];
    g.shadow.fx[0] = params[4];
    g.shadow.fx[1] = params[5];
    std::memcpy(g.shadow.lights, params + 12, size_t(count) * 8 * sizeof(float));
    if (g.shadowFrame) {
        g.shadow.params[1] *= params[8];
        g.shadow.params[2] *= params[9];
    }
}
void Sky(const float* params) {
    if (!g.ready || !g.shadowScene) return;
    std::memcpy(g.atmosphere.sky0, params, 4 * sizeof(float));
    std::memcpy(g.atmosphere.sky1, params + 4, 4 * sizeof(float));
    std::memcpy(g.atmosphere.sun, params + 8, 4 * sizeof(float));
    std::memcpy(g.atmosphere.sunColor, params + 12, 4 * sizeof(float));
}
void PostEffects(const float* params) {
    if (!g.ready) return;
    std::memcpy(g.postFx, params, sizeof(g.postFx));
}
void ShadowCasting(bool enabled) { g.shadowCasting = enabled; }
void MapObjects(bool active) { g.mapObjects = active; }
// El posproceso abre una pasada: al codificarla se aplica antes de sus dibujos.
void PostProcess(bool antialias, float sharpen) {
    if (!g.ready) return;
    g.passes.emplace_back();
    auto& pass = g.passes.back();
    pass.antialias = antialias;
    pass.sharpen = sharpen;
    std::memcpy(pass.fx, g.postFx, sizeof(pass.fx));
    std::memset(g.postFx, 0, sizeof(g.postFx));
    std::memcpy(pass.postViewport, RenderState::Get().viewport, sizeof(pass.postViewport));
}
void ReadPixelsRGB(int x, int y, int width, int height, void* out) {
    if (out && width > 0 && height > 0) ::WebGPU::ReadPixels(x, y, width, height, out);
}
int MaxTextureSize() { return 8192; }
void MaxViewportDims(int out[2]) { out[0] = out[1] = 8192; }
const char* GetDeviceInfo(DeviceInfo info) {
    switch (info) {
    case DeviceInfo::Vendor: return "WebGPU";
    case DeviceInfo::Renderer: return ::WebGPU::Description();
    default: return "Dawn";
    }
}
Texture CreateTexture() {
    const Texture handle = g.nextTexture++;
    g.textures.emplace(handle, ::WebGPU::TextureSlot{});
    return handle;
}
void DeleteTexture(Texture texture) {
    auto it = g.textures.find(texture);
    if (it == g.textures.end()) return;
    if (it->second.usedFrame == g.frame) ::WebGPU::Retire(it->second);
    else ::WebGPU::Destroy(it->second);
    g.textures.erase(it);
}
void TexImage2D(int, int width, int height, Format format, const void* pixels) {
    if (!g.ready || width <= 0 || height <= 0) return;
    ::WebGPU::TextureSlot* slot = ::WebGPU::Writable(false);
    if (!slot) return;
    if (!slot->texture || slot->width != width || slot->height != height) {
        if (slot->usedFrame == g.frame) ::WebGPU::Retire(*slot); else ::WebGPU::Destroy(*slot);
        slot->texture = ::WebGPU::NewTexture(width, height);
        slot->view = wgpuTextureCreateView(slot->texture, nullptr);
        slot->width = width; slot->height = height;
    }
    auto rgba = ::WebGPU::ToRGBA(width, height, format, pixels);
    ::WebGPU::Write(slot->texture, 0, 0, width, height, rgba);
    if (Gfx::TextureMipmaps()) ::WebGPU::WriteMipmaps(slot->texture, width, height, std::move(rgba));
}
void TexSubImage2D(int x, int y, int width, int height, Format format, const void* pixels) {
    if (!g.ready || width <= 0 || height <= 0) return;
    ::WebGPU::TextureSlot* slot = ::WebGPU::Writable(true);
    if (!slot || !slot->texture || x < 0 || y < 0 || x + width > slot->width || y + height > slot->height) return;
    // Sólo el nivel 0: las texturas que se actualizan por partes (texto,
    // marcas) usan filtro nítido, que no muestrea los mipmaps.
    ::WebGPU::Write(slot->texture, x, y, width, height, ::WebGPU::ToRGBA(width, height, format, pixels));
}
void TexParameter(TexParam parameter, TexValue value) {
    auto it = g.textures.find(RenderState::Get().texture);
    if (it == g.textures.end()) return;
    auto& s = it->second.sampling;
    switch (parameter) {
    case TexParam::MinFilter: s.minFilter = value; break;
    case TexParam::MagFilter: s.magFilter = value; break;
    case TexParam::WrapS: s.wrapS = value; break;
    case TexParam::WrapT: s.wrapT = value; break;
    }
}
void TexBorderColor(const float*) {}
void SetUnpackAlignment(int alignment) { g.unpackAlignment = alignment; }
int UnpackAlignment() { return g.unpackAlignment; }
}

// ── BufferedDraw ────────────────────────────────────────────────────────────
namespace BufferedDraw::WGPU {
using ::WebGPU::g;
bool Initialize() {
    if (g.ready) return true;
    if (!::WebGPU::IsReady() || !::WebGPU::CreateObjects()) { ::WebGPU::ReleaseObjects(); return false; }
    g.ready = true;
    Gfx::ResetState();
    return true;
}
void Release() { if (g.ready) ::WebGPU::ReleaseObjects(); }
void Submit(unsigned mode, const Vertex* vertices, std::size_t count) {
    if (!g.ready || !count) return;
    const auto& s = RenderState::Get();
    const bool quads = mode == ::WebGPU::Quads;
    WGPUPrimitiveTopology topology;
    switch (mode) {
    case ::WebGPU::Points: topology = WGPUPrimitiveTopology_PointList; break;
    case ::WebGPU::Lines: topology = WGPUPrimitiveTopology_LineList; break;
    case ::WebGPU::LineLoop:
    case ::WebGPU::LineStrip: topology = WGPUPrimitiveTopology_LineStrip; break;
    case ::WebGPU::TriangleStrip: topology = WGPUPrimitiveTopology_TriangleStrip; break;
    default: topology = WGPUPrimitiveTopology_TriangleList; break;
    }
    const ::WebGPU::PipelineKey key = ::WebGPU::StateKey(uint8_t(topology));
    ::WebGPU::TextureSlot* slot = &::WebGPU::BoundSlot();
    ::WebGPU::Draw draw{};
    draw.pipeline = ::WebGPU::Pipeline(key);
    draw.textureGroup = ::WebGPU::TextureGroup(*slot);
    slot->usedFrame = g.frame;
    draw.uniformOffset = ::WebGPU::UniformOffset();
    draw.firstVertex = uint32_t(g.vertices.size());
    draw.quads = quads;
    draw.caster = ::WebGPU::Caster(key);
    draw.overWater = ::WebGPU::OverWater(key);
    draw.topology = uint8_t(topology);
    std::memcpy(draw.viewport, s.viewport, sizeof(draw.viewport));
    g.vertices.insert(g.vertices.end(), vertices, vertices + count);
    if (mode == ::WebGPU::LineLoop) g.vertices.push_back(vertices[0]);   // cerrar el lazo
    draw.vertexCount = uint32_t(g.vertices.size()) - draw.firstVertex;
    // Un lote igual al anterior y pegado a él se suma a ese dibujo (no en
    // strips: unirlos conectaría primitivas que no lo están).
    auto& draws = ::WebGPU::CurrentPass().draws;
    const bool list = key.topology != WGPUPrimitiveTopology_LineStrip && key.topology != WGPUPrimitiveTopology_TriangleStrip;
    if (list && !draws.empty()) {
        auto& last = draws.back();
        if (!last.vertexBuffer && last.pipeline == draw.pipeline && last.textureGroup == draw.textureGroup &&
            last.uniformOffset == draw.uniformOffset && last.quads == draw.quads && last.caster == draw.caster &&
            last.overWater == draw.overWater &&
            last.firstVertex + last.vertexCount == draw.firstVertex &&
            !std::memcmp(last.viewport, draw.viewport, sizeof(draw.viewport))) {
            last.vertexCount += draw.vertexCount;
            return;
        }
    }
    draws.push_back(draw);
}
void Suspend() {}
// Mallas residentes y skinning de GL: en WebGPU llegan en una etapa siguiente.
void Activate(unsigned, unsigned, unsigned) {}
void DrawIndexed(unsigned, unsigned, const Vertex*, std::size_t, std::size_t) {}
}
