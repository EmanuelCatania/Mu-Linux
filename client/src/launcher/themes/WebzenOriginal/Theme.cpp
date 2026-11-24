// Tema WebzenOriginal (ver README.md): el launcher de controles de Windows,
// medidas de su Forms/Main.Designer.cs.  Ventana con la barra de título del
// sistema, banner arriba, barra en bloques y botones grises.

#include "ThemeSpec.h"

namespace Launcher {

ThemeSpec MakeTheme()
{
	const SDL_Color control{ 240, 240, 240, 255 };   // SystemColors.Control

	ThemeSpec t;
	t.name = "WebzenOriginal";
	t.width = 294;
	t.height = 386;
	t.nativeFrame = true;                          // FormBorderStyle.FixedSingle
	t.clearColor = control;

	// El banner va estirado arriba, como en el original (panel1 + pictureBox1).
	t.decor = { { "Banner", { 1, 1, 292, 54 } } };
	t.showTitle = false;                           // lo dice la barra de título
	// En el original ahí estaban las opciones (Function_panel); acá, el banner
	// con las noticias del server.  No muestra la web: tiene el botón Web.
	t.imagePanel = { 1, 59, 292, 244 };
	t.image = "Banner";
	t.webPanel = false;
	t.bar.rect = { 10, 323, 269, 22 };             // Complete_panel, Fixed3D
	t.bar.track = control;
	t.bar.segmented = true;                        // SegmentedProgressBar
	t.status = { 10, 307, 269, 0 };
	t.pause = { 229, 306, 50, 14 };
	t.pauseFill = { 225, 225, 225, 255 };
	t.buttonStyle.system = true;
	t.buttonStyle.textDisabled = { 160, 160, 160, 255 };
	// Play, Close y Web del original, con Opciones en el medio (las opciones ya
	// no entran en la ventana).
	t.play = { 10, 351, 65, 30 };
	t.options = { 79, 351, 65, 30 };
	t.close = { 148, 351, 65, 30 };
	t.web = { 217, 351, 65, 30 };
	t.credit = { 218, 42 };
	t.creditColor = { 255, 255, 255, 255 };

	// Las opciones no entran en 294x386: mientras están abiertas la ventana
	// crece.
	t.optionsWidth = 450;
	t.optionsHeight = 420;
	t.optionsTitlePos = { 10, 8 };
	t.optionsPanel = { 8, 30, 434, 340 };
	t.messageY = 374;
	t.save = { 150, 384, 75, 28 };
	t.back = { 233, 384, 75, 28 };
	t.palette.panel = control;
	t.palette.rowAlt = { 230, 230, 230, 255 };
	return t;
}

} // namespace Launcher
