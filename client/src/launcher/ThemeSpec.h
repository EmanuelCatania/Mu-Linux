#pragma once

// Disposición y colores de un tema del launcher (los diseños de kayito, ver
// themes/<tema>/README.md).  Cada tema la arma en su themes/<tema>/Theme.cpp;
// el tema se elige al compilar (-DMU_LAUNCHER_THEME=<tema>) y sólo sus imágenes
// van adentro del ejecutable.
//
// Las imágenes se nombran por su archivo sin extensión ("Button_n" es
// themes/<tema>/Button_n.png).  Un nombre vacío es "sin imagen".  Las medidas
// son en unidades lógicas: la ventana se dibuja a ese tamaño y se escala entera.

#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace Launcher {

struct ButtonStyle
{
	std::string normal, hover, disabled, pressed;   // las que faltan caen en normal
	SDL_Color text{ 0, 0, 0, 255 };
	SDL_Color textDisabled{ 70, 70, 70, 255 };
	float fontSize = 0;                    // 0: la del resto de los botones
	bool bold = true;                      // con fontSize: negrita o regular
	bool textTop = false;                  // texto arriba (TopCenter del original) en vez de centrado
	bool noLabel = false;                  // botón con ícono: la imagen sola
	bool system = false;                   // botón gris de Windows, sin imágenes
};

// Una imagen fija sobre el fondo (logo, ícono de la cabecera).
struct ThemeImage
{
	std::string name;
	SDL_FRect rect{};
};

struct ProgressBarSpec
{
	SDL_FRect rect{};                      // ancho 0: no hay barra
	bool bevel = true;                     // borde 3D hundido, el Fixed3D del original
	SDL_Color track{ 240, 240, 240, 255 }; // alfa 0: sin fondo (lo trae la imagen del tema)
	SDL_Color fill{ 0, 120, 215, 255 };
	SDL_Color failed{ 190, 40, 40, 255 };
	std::string image;                     // relleno con imagen en vez de color
	SDL_FPoint percentPos{};               // "37%"; x 0: no se muestra
	SDL_Color percentColor{ 0, 0, 0, 255 };
	bool segmented = false;                // en bloques, como la ProgressBar clásica de Windows
};

// Colores de la pantalla de opciones.
struct OptionsPalette
{
	SDL_Color panel{ 224, 224, 224, 255 };   // tapa los recuadros fijos del fondo
	SDL_Color tabOn{ 250, 250, 250, 255 };
	SDL_Color tabOff{ 205, 205, 205, 255 };
	SDL_Color rowAlt{ 232, 232, 232, 255 };
	SDL_Color text{ 0, 0, 0, 255 };
	SDL_Color dim{ 70, 70, 70, 255 };        // pestaña apagada, valor que no se puede cambiar
	SDL_Color faint{ 120, 120, 120, 255 };   // flechas, "(ninguno)"
	SDL_Color box{ 255, 255, 255, 255 };     // fondo de los campos de texto
	SDL_Color title{ 0, 0, 0, 255 };
};

struct ThemeSpec
{
	const char* name = "";
	int width = 0, height = 0;
	SDL_FRect drag{};                      // lo que arrastra la ventana
	bool nativeFrame = false;              // ventana con la barra de título del sistema
	// La ventana toma la forma del fondo: lo transparente de MainBackground no
	// se dibuja ni recibe clics (el TransparencyKey del original).
	bool shaped = false;

	// Pantalla principal.
	std::string background;                // estirada a toda la ventana
	SDL_Color clearColor{ 224, 224, 224, 255 };   // donde no hay fondo
	std::vector<ThemeImage> decor;         // encima del fondo
	bool showTitle = true;
	SDL_FPoint titlePos{};
	SDL_Color titleColor{ 0, 0, 0, 255 };
	SDL_FRect imagePanel{};                // la imagen, las noticias y la web del server
	bool webPanel = true;                  // false: el tema no muestra la web ahí
	bool imageBevel = true;
	std::string image;                     // vacío: se ve el fondo
	ProgressBarSpec bar;                   // lo bajado en bytes
	ProgressBarSpec fileBar;               // archivos bajados (los temas con dos barras)
	SDL_FRect status{};                    // x, y y ancho de la línea de estado
	bool statusCentered = false;
	SDL_Color statusColor{ 0, 0, 0, 255 };
	SDL_Color statusError{ 150, 0, 0, 255 };
	SDL_FRect pause{};                     // botón Pausar, a la derecha del estado
	SDL_Color pauseFill{ 225, 225, 225, 255 }, pauseHover{ 250, 250, 250, 255 }, pauseText{ 0, 0, 0, 255 };
	ButtonStyle buttonStyle;
	ButtonStyle playStyle;                 // normal vacío: la de los demás
	ButtonStyle optionsStyle;              // ídem (en algunos temas es un ícono)
	SDL_FRect options{}, play{}, close{};  // ancho 0: no hay ese botón (close)
	SDL_FRect web{};                       // botón "Web" en la principal (ancho 0: está en Opciones)
	SDL_FRect exit{};                      // la X de cerrar (ancho 0: no hay)
	ButtonStyle exitStyle;
	SDL_FPoint credit{};                   // "Diseño: kayito"
	SDL_Color creditColor{ 70, 70, 70, 255 };

	// Opciones.  En los temas donde eran otra ventana, la ventana toma su
	// tamaño mientras están abiertas.
	int optionsWidth = 0, optionsHeight = 0;   // 0: el de la principal
	SDL_FRect optionsDrag{};                   // ancho 0: el de la principal
	std::string optionsBackground;
	bool showOptionsTitle = true;
	SDL_FPoint optionsTitlePos{};
	SDL_FRect optionsPanel{};
	int tabsPerRow = 4;
	float messageY = 0;
	SDL_FRect save{}, back{};
	ButtonStyle saveStyle, backStyle;      // normal vacío: la de los demás
	OptionsPalette palette;

	const ButtonStyle& Or(const ButtonStyle& s) const { return s.normal.empty() ? buttonStyle : s; }
	const ButtonStyle& PlayStyle() const { return Or(playStyle); }
	int OptionsWidth() const { return optionsWidth ? optionsWidth : width; }
	int OptionsHeight() const { return optionsHeight ? optionsHeight : height; }
};

// La del tema compilado (themes/<tema>/Theme.cpp).
ThemeSpec MakeTheme();

} // namespace Launcher
