// Tema WebzenModern (ver README.md): medidas y colores de su
// Forms/Main.Designer.cs y Forms/Options.Designer.cs.  Ventana de 1024x576
// con la imagen grande, íconos de opciones y cerrar en la cabecera y las dos
// barras abajo, junto al logo.

#include "ThemeSpec.h"

namespace Launcher {

ThemeSpec MakeTheme()
{
	const SDL_Color steel{ 176, 196, 222, 255 };   // Color.LightSteelBlue

	ThemeSpec t;
	t.name = "WebzenModern";
	t.width = 1024;
	t.height = 576;
	t.drag = { 0, 0, 1024, 29 };                   // Header_panel

	t.background = "MainBackground";
	t.decor = { { "MuIcon", { 6, 6, 18, 18 } }, { "MuLogo", { 6, 517, 109, 53 } } };
	t.titlePos = { 34, 8 };
	t.titleColor = steel;
	t.imagePanel = { 6, 30, 1012, 481 };           // Main_Image / Web_panel
	t.imageBevel = false;
	t.image = "MainImage";
	// Update_panel en 121,517.  Como en el original, la barra de lo bajado usa
	// CurrentProgress y la de archivos CompleteProgress.
	t.bar.rect = { 133, 553, 670, 12 };
	t.bar.bevel = false;
	t.bar.track = { 22, 24, 26, 255 };
	t.bar.image = "CurrentProgress";
	t.bar.percentPos = { 809, 552 };
	t.bar.percentColor = steel;
	t.fileBar = t.bar;
	t.fileBar.rect = { 133, 537, 670, 12 };
	t.fileBar.image = "CompleteProgress";
	t.fileBar.percentPos = { 809, 535 };
	t.status = { 133, 520, 678, 0 };
	t.statusColor = steel;
	t.statusError = { 255, 120, 120, 255 };
	t.pause = { 757, 519, 46, 13 };
	t.pauseFill = { 25, 35, 45, 255 };
	t.pauseHover = { 40, 60, 80, 255 };
	t.pauseText = steel;
	t.buttonStyle.normal = "Accept_n";
	t.buttonStyle.hover = "Accept_h";
	t.buttonStyle.pressed = "Accept_c";
	t.buttonStyle.text = steel;
	t.buttonStyle.textDisabled = { 110, 125, 140, 255 };
	t.playStyle.normal = "Jugar_n";
	t.playStyle.hover = "Jugar_h";
	t.playStyle.pressed = "Jugar_c";
	t.playStyle.disabled = "Jugar_d";
	t.playStyle.text = steel;
	t.playStyle.textDisabled = { 110, 125, 140, 255 };
	t.playStyle.fontSize = 19;                     // Verdana 14 pt
	t.playStyle.bold = false;
	t.optionsStyle.normal = "Config_n";
	t.optionsStyle.hover = "Config_h";
	t.optionsStyle.pressed = "Config_c";
	t.optionsStyle.noLabel = true;
	t.options = { 976, 6, 18, 18 };
	t.play = { 872, 517, 146, 53 };
	t.close = { 0, 0, 0, 0 };                      // se cierra con la X
	t.exit = { 1000, 6, 18, 18 };
	t.exitStyle.normal = "Cerrar_n";
	t.exitStyle.hover = "Cerrar_h";
	t.exitStyle.pressed = "Cerrar_c";
	t.credit = { 890, 10 };
	t.creditColor = { 110, 125, 140, 255 };

	// Las opciones eran una ventana aparte de 446x461.
	t.optionsWidth = 446;
	t.optionsHeight = 461;
	t.optionsDrag = { 0, 0, 446, 30 };
	t.optionsBackground = "OptionsBackground";
	t.optionsTitlePos = { 12, 8 };
	t.optionsPanel = { 12, 35, 422, 362 };
	t.messageY = 394;
	t.save = { 170, 410, 108, 43 };                // Btn_Save
	t.back = { 422, 6, 18, 18 };                   // Btn_Close: la X
	t.backStyle = t.exitStyle;
	t.backStyle.noLabel = true;
	OptionsPalette& p = t.palette;
	p.panel = { 0, 0, 0, 0 };                      // el fondo ya es oscuro
	p.tabOn = { 30, 60, 90, 255 };
	p.tabOff = { 14, 22, 32, 255 };
	p.rowAlt = { 255, 255, 255, 12 };
	p.text = steel;
	p.dim = { 110, 125, 140, 255 };
	p.faint = { 110, 125, 140, 255 };
	p.box = { 10, 14, 20, 255 };
	p.title = steel;
	return t;
}

} // namespace Launcher
