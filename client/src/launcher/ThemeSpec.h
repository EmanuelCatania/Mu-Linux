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

namespace Launcher {

struct ButtonStyle
{
	std::string normal, hover, disabled;   // la de hover y la de deshabilitado caen en normal si faltan
	SDL_Color text{ 0, 0, 0, 255 };
	SDL_Color textDisabled{ 70, 70, 70, 255 };
};

struct ProgressBarSpec
{
	SDL_FRect rect{};
	bool bevel = true;                     // borde 3D hundido, el Fixed3D del original
	SDL_Color track{ 240, 240, 240, 255 };
	SDL_Color fill{ 0, 120, 215, 255 };
	SDL_Color failed{ 190, 40, 40, 255 };
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
	float dragHeight = 0;                  // franja de arriba que arrastra la ventana

	// Pantalla principal.
	std::string background;                // estirada a toda la ventana
	SDL_FPoint titlePos{};
	SDL_Color titleColor{ 0, 0, 0, 255 };
	SDL_FRect imagePanel{};                // la imagen y las noticias del server
	bool imageBevel = true;
	std::string image;
	ProgressBarSpec bar;
	SDL_FRect status{};                    // x, y y ancho de la línea de estado
	SDL_Color statusColor{ 0, 0, 0, 255 };
	SDL_Color statusError{ 150, 0, 0, 255 };
	SDL_FRect pause{};                     // botón Pausar, a la derecha del estado
	SDL_Color pauseFill{ 225, 225, 225, 255 }, pauseHover{ 250, 250, 250, 255 }, pauseText{ 0, 0, 0, 255 };
	ButtonStyle buttonStyle;
	SDL_FRect options{}, play{}, close{};
	SDL_FPoint credit{};                   // "Diseño: kayito"
	SDL_Color creditColor{ 70, 70, 70, 255 };

	// Opciones.
	std::string optionsBackground;
	SDL_FPoint optionsTitlePos{};
	SDL_FRect optionsPanel{};
	float messageY = 0;
	SDL_FRect save{}, back{};
	OptionsPalette palette;
};

// La del tema compilado (themes/<tema>/Theme.cpp).
ThemeSpec MakeTheme();

} // namespace Launcher
