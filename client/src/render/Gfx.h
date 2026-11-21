#pragma once
#include <cstdint>

// Capa de render: el estado y los recursos que usa el juego, sin tipos ni
// constantes de OpenGL. El estado del pipeline fijo (alpha test, niebla,
// texture env, matrices, color actual) vive en Render/RenderState y lo
// consumen los shaders; el backend sólo envía al dispositivo el estado que
// existe en OpenGL 3.3 core. Antes de cada cambio se envía el lote pendiente,
// así el orden de dibujo no cambia. La geometría va por BufferedDraw; esto
// cubre estado, matrices, texturas, niebla y consultas.
namespace Gfx {

enum class Cap { Texture2D, Blend, AlphaTest, DepthTest, CullFace, Fog, ScissorTest, StencilTest, Lighting, Dither, PolygonSmooth, ColorLogicOp };
enum class Factor { Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstAlpha, OneMinusDstAlpha, DstColor, OneMinusDstColor, SrcAlphaSaturate };
enum class Compare { Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always };
enum class Matrix { ModelView, Projection, Texture };
enum class Format { RGB, RGBA };
enum class TexParam { MinFilter, MagFilter, WrapS, WrapT };
enum class TexValue { Nearest, Linear, NearestMipmapNearest, LinearMipmapNearest, NearestMipmapLinear, LinearMipmapLinear, Repeat, Clamp, ClampToEdge };
enum class TexEnv { Modulate, Replace, Decal, Blend, Add };
enum class FogMode { Linear, Exp, Exp2 };
enum class DeviceInfo { Vendor, Renderer, Version };
// Máscaras combinables con |.
enum ClearMask : unsigned { ClearColorBuffer = 1, ClearDepthBuffer = 2, ClearStencilBuffer = 4 };
enum AttribMask : unsigned { AttribCurrent = 1, AttribTexture = 2, AttribEnable = 4,
    AttribColorBuffer = 16, AttribViewport = 32, AttribFog = 64, AttribDepth = 128 };

// Al crear un contexto: deja RenderState y el dispositivo con los valores
// iniciales de OpenGL.
void ResetState();

// Estado
void Enable(Cap cap);
void Disable(Cap cap);
inline void SetEnabled(Cap cap, bool on) { on ? Enable(cap) : Disable(cap); }
bool IsEnabled(Cap cap);
void BlendFunc(Factor source, Factor destination);
void DepthFunc(Compare function);
void DepthMask(bool write);
void AlphaFunc(Compare function, float reference);
void PushAttrib(unsigned attribMask);
void PopAttrib();

// Color actual (el que toma BufferedDraw al empezar una primitiva)
void Color3f(float r, float g, float b);
void Color4f(float r, float g, float b, float a);
void Color4fv(const float* rgba);
void Color4ub(unsigned char r, unsigned char g, unsigned char b, unsigned char a);
void CurrentColor(float rgba[4]);

// Matrices
void MatrixMode(Matrix mode);
void PushMatrix();
void PopMatrix();
void LoadIdentity();
void Translate(float x, float y, float z);
void Rotate(float degrees, float x, float y, float z);
void Ortho2D(double left, double right, double bottom, double top);
void Perspective(double fovy, double aspect, double zNear, double zFar);
// Matriz actual de la pila indicada, por columnas (16 floats).
void GetMatrix(Matrix which, float out[16]);

// Marco
void Viewport(int x, int y, int width, int height);
void ClearColor(float r, float g, float b, float a);
void Clear(unsigned clearMask);
void Flush();
// Descarta los errores pendientes del dispositivo (p. ej. tras pops de más).
void ClearErrors();
// Lee el framebuffer en RGB de 8 bits, filas de abajo hacia arriba.
void ReadPixelsRGB(int x, int y, int width, int height, void* out);
// DESVIACION: posproceso opcional sobre el viewport actual, con lo dibujado
// hasta ahora: antialiasing FXAA y después nitidez adaptativa (tipo AMD CAS).
// sharpen: 0 apagada, 1 = máximo de AMD CAS.
void PostProcess(bool antialias, float sharpen);
int MaxTextureSize();
void MaxViewportDims(int out[2]);
const char* GetDeviceInfo(DeviceInfo info);

// Texturas (handles propios; hoy coinciden con los nombres del backend)
using Texture = unsigned;
Texture CreateTexture();
void DeleteTexture(Texture texture);
void BindTexture(Texture texture);
// components: cantidad de canales almacenados (3 o 4), como el original.
void TexImage2D(int components, int width, int height, Format format, const void* pixels);
// Actualiza un rectángulo de la textura ligada (filas contiguas, sin relleno).
void TexSubImage2D(int x, int y, int width, int height, Format format, const void* pixels);
void TexParameter(TexParam parameter, TexValue value);
// Los cargadores del 0.97k reciben filtro y wrap con los valores numéricos
// del binario (p. ej. OpenJPG(..., 0x2601, 0x2901)); esto los traduce.
TexValue TexValueFromLegacy(int value);
void TexBorderColor(const float rgba[4]);
void TexEnvMode(TexEnv mode);
void SetUnpackAlignment(int alignment);
int UnpackAlignment();

// DESVIACION: calidad de texturas, opción del jugador. Los mipmaps se generan
// al subir cada textura (se fija al arrancar); el bicúbico cambia en caliente.
void SetTextureQuality(bool mipmaps, bool bicubic);
bool TextureMipmaps();
bool TextureBicubic();
// DESVIACION: muestras de MSAA del framebuffer (0 sin MSAA); se fija al crear la ventana.
void SetMultisample(int samples);
// DESVIACION: versiones -hd de texturas y modelos; se fija al arrancar.
void SetHDTextures(bool enabled);
bool HDTextures();
int Multisample();
// DESVIACION: sombras proyectadas con mapa de sombras (opción del jugador). El
// mundo 3D del cuadro va entre BeginShadowScene y EndShadowScene: lo que se
// dibuja adentro proyecta y recibe sombra. focus es el centro (el héroe).
// ShadowMaps() es verdadero si la opción está activa y el backend la soporta.
void SetShadowMaps(bool enabled, float strength);
bool ShadowMaps();
void BeginShadowScene(const float focus[3]);
void EndShadowScene();
// Dentro de la escena, lo dibujado con casting en false recibe sombra pero no
// la proyecta (p. ej. el pasto). Cada escena empieza con casting en true.
void SetShadowCasting(bool enabled);
// DESVIACION: niebla por profundidad (opción). Con la opción activa la escena
// se marca aunque no haya sombras. Atmosphere se fija por cuadro, antes de
// EndShadowScene: mode 0 nada, 1 bruma de distancia, 2 bajo el agua (absorbe
// primero el rojo); start = distancia desde la que empieza.
void SetDepthFog(bool enabled);
bool DepthFog();
void Atmosphere(int mode, const float color[3], float density, float start);
// DESVIACION (prueba): mar con olas, Fresnel y brillo del sol sobre el plano
// z = level, en una pasada que se aplica antes de lo que se dibuje después.
// Sólo WebGPU; con GL o la opción apagada no hace nada. sun = posición en el
// mundo; time en segundos. backdropY = y de la pared pintada del horizonte: el
// agua se funde con el cielo al acercarse a ella en vez de cortar.
void Water(float level, float time, const float sun[3], float backdropY);
// Luz puntual que el próximo Water() refleja sobre el agua; hasta 16 por
// cuadro. compact = false: columna estirada por las olas (faroles); true:
// charco de luz redondo en el agua justo debajo, que no se dibuja si hay
// cubierta en medio (el hada).
void WaterLight(const float position[3], const float color[3], bool compact = false);
// Barco que el próximo Water() acompaña con olas de proa en V; hasta 4.
void WaterShip(const float bow[2], const float heading[2]);
// Altura (y pendiente, si se pide) del mar de fondo en (x, y): la misma onda
// larga que dibuja el agua, para que los barcos se mezan con ella.
float WaterSwell(float x, float y, float time, float* slopeX, float* slopeY);
void ToggleWater();
bool WaterEnabled();
// Agua de los mapas: se llama justo después de dibujar el terreno. mask dice
// dónde hay agua y cuánta luz tiene (256x256 RGBA; ver WaterProfiles); el
// pase dibuja encima el agua nueva con olas, cielo y sol. Se sube al
// dispositivo sólo cuando cambia.
struct WaterLook {
    float tint[3];   // color del cuerpo de agua
    float sky[3];    // color que refleja
    float waves;     // fuerza de las olas (1 = las del login)
    float sun[3];    // dirección hacia el sol, en el mundo
    // 1: el agua plana se redibuja entera (el terreno no pinta la vieja) con
    // la orilla suave de B; 0: va sobre el agua que pinta el mapa.
    float redraw = 0.0f;
};
// focus: el centro del área donde el agua simula ondas (el héroe).
void WorldWater(const unsigned char* mask, float time, const WaterLook& look, const float focus[2]);
// Fuego del mundo que el agua de los mapas refleja (hasta 16, los más nuevos);
// con ripple, en cambio, algo que toca el agua: empuja la superficie (las ondas
// las simula el agua) con fuerza color[0] en un radio color[1].
// Lo registra la simulación, que no corre en cada cuadro: cada luz dura 0.3 s
// desde la última vez que se registró, así no titila entre ticks.
void WorldWaterLight(const float position[3], const float color[3], bool ripple = false);

// Niebla
void SetFogMode(FogMode mode);
void FogStart(float value);
void FogEnd(float value);
void FogDensity(float value);
void FogColor(const float rgba[4]);

}
