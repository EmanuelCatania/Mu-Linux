// Tema WebzenOldSchoolV2 (ver README.md): medidas y colores de su
// Forms/Main.Designer.cs.  Es la versión oscura del V1.

#include "ThemeSpec.h"

namespace Launcher {

ThemeSpec MakeTheme()
{
	const SDL_Color control{ 240, 240, 240, 255 };   // SystemColors.Control

	ThemeSpec t;
	t.name = "WebzenOldSchoolV2";
	t.width = 450;
	t.height = 436;
	t.drag = { 0, 0, 450, 26 };

	t.background = "MainBackground";
	t.titlePos = { 5, 6 };
	t.titleColor = control;
	t.imagePanel = { 6, 32, 437, 327 };
	t.image = "MainImage";
	t.webPanel = false;                            // la web, desde Opciones
	t.bar.rect = { 6, 365, 437, 22 };
	t.bar.track = { 34, 34, 34, 255 };
	t.bar.fill = { 153, 180, 209, 255 };   // SystemColors.ActiveCaption
	t.status = { 6, 390, 437, 0 };
	t.statusColor = { 255, 255, 255, 255 };
	t.statusError = { 255, 120, 120, 255 };
	t.pause = { 373, 390, 70, 15 };
	t.pauseFill = { 60, 60, 60, 255 };
	t.pauseHover = { 90, 90, 90, 255 };
	t.pauseText = control;
	t.buttonStyle.normal = "Button_n";
	t.buttonStyle.hover = "Button_h";
	t.buttonStyle.disabled = "Button_d";
	t.buttonStyle.text = control;
	t.buttonStyle.textDisabled = { 130, 130, 130, 255 };
	t.options = { 46, 407, 90, 28 };
	t.play = { 221, 407, 90, 28 };
	t.close = { 311, 407, 90, 28 };
	t.credit = { 143, 416 };
	t.creditColor = { 150, 150, 150, 255 };

	t.optionsBackground = "OptionsBackground";
	t.optionsTitlePos = { 6, 6 };
	t.optionsPanel = { 9, 36, 431, 360 };
	t.messageY = 382;
	t.save = { 130, 403, 90, 28 };
	t.back = { 230, 403, 90, 28 };
	OptionsPalette& p = t.palette;
	p.panel = { 0, 0, 0, 215 };           // deja ver apenas el logo del fondo
	p.tabOn = { 75, 75, 75, 255 };
	p.tabOff = { 40, 40, 40, 255 };
	p.rowAlt = { 30, 30, 30, 200 };
	p.text = control;
	p.dim = { 150, 150, 150, 255 };
	p.faint = { 120, 120, 120, 255 };
	p.box = { 20, 20, 20, 255 };
	p.title = control;
	return t;
}

} // namespace Launcher
