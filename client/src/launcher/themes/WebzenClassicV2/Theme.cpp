// Tema WebzenClassicV2 (ver README.md): medidas y colores de su
// Forms/Main.Designer.cs y Forms/Options.Designer.cs.  Ventana con la forma
// del fondo (el caballero sobresale del marco), dos barras con imagen.
// Igual que el V1 con la paleta dorada.

#include "ThemeSpec.h"

namespace Launcher {

ThemeSpec MakeTheme()
{
	const SDL_Color steel{ 218, 165, 32, 255 };    // Color.Goldenrod

	ThemeSpec t;
	t.name = "WebzenClassicV2";
	t.width = 792;
	t.height = 582;
	t.drag = { 1, 152, 791, 71 };                  // Header_panel
	t.shaped = true;

	t.background = "MainBackground";
	t.showTitle = false;
	t.imagePanel = { 25, 225, 749, 284 };          // Web_panel
	t.imageBevel = false;
	// Update_panel en 29,515; las barras y los textos van relativos a él.
	t.bar.rect = { 83, 538, 368, 8 };
	t.bar.bevel = false;
	t.bar.track = { 0, 0, 0, 0 };
	t.bar.image = "CompleteProgress";
	t.bar.percentPos = { 454, 534 };
	t.bar.percentColor = steel;
	t.fileBar = t.bar;
	t.fileBar.rect = { 83, 549, 368, 8 };
	t.fileBar.image = "CurrentProgress";
	t.fileBar.percentPos = { 454, 547 };
	t.status = { 80, 521, 368, 0 };
	t.statusCentered = true;
	t.statusColor = { 255, 215, 0, 255 };          // Color.Gold
	t.statusError = { 255, 120, 120, 255 };
	t.pause = { 454, 519, 46, 13 };
	t.pauseFill = { 45, 25, 15, 255 };
	t.pauseHover = { 75, 45, 25, 255 };
	t.pauseText = steel;
	t.buttonStyle.normal = "Boton_n";
	t.buttonStyle.hover = "Boton_h";
	t.buttonStyle.text = steel;
	t.buttonStyle.textDisabled = { 125, 100, 70, 255 };
	t.playStyle.normal = "Jugar_n";
	t.playStyle.hover = "Jugar_h";
	t.playStyle.disabled = "Jugar_d";
	t.playStyle.text = { 184, 134, 11, 255 };      // Color.DarkGoldenrod
	t.playStyle.textDisabled = { 110, 85, 50, 255 };
	t.playStyle.fontSize = 29;                     // Ubuntu 22 pt
	t.options = { 518, 515, 74, 22 };
	t.close = { 518, 542, 74, 22 };
	t.play = { 615, 515, 159, 49 };
	t.exit = { 751, 198, 22, 20 };
	t.exitStyle.normal = "Cerrar_n";
	t.exitStyle.hover = "Cerrar_h";
	t.credit = { 655, 203 };
	t.creditColor = { 140, 110, 70, 255 };

	// Las opciones eran una ventana aparte, con su fondo de 385x451.
	t.optionsWidth = 385;
	t.optionsHeight = 451;
	t.optionsDrag = { 0, 0, 385, 55 };
	t.optionsBackground = "OptionsBackground";
	t.showOptionsTitle = false;                    // el fondo trae el logo
	t.optionsPanel = { 18, 60, 349, 322 };
	t.tabsPerRow = 3;
	t.messageY = 386;
	t.back = { 82, 402, 77, 24 };
	t.save = { 226, 402, 77, 24 };
	OptionsPalette& p = t.palette;
	p.panel = { 0, 0, 0, 110 };
	p.tabOn = { 95, 55, 25, 255 };
	p.tabOff = { 40, 22, 12, 255 };
	p.rowAlt = { 255, 255, 255, 18 };
	p.text = steel;
	p.dim = { 205, 92, 92, 255 };          // Color.IndianRed
	p.faint = { 150, 110, 70, 255 };
	p.box = { 25, 12, 6, 255 };
	p.title = { 255, 215, 0, 255 };
	return t;
}

} // namespace Launcher
