// Tema WebzenOldSchoolV1 (ver README.md): medidas de su Forms/Main.Designer.cs.

#include "ThemeSpec.h"

namespace Launcher {

ThemeSpec MakeTheme()
{
	ThemeSpec t;
	t.name = "WebzenOldSchoolV1";
	t.width = 450;
	t.height = 458;
	t.drag = { 0, 0, 450, 26 };

	t.background = "MainBackground";
	t.titlePos = { 5, 6 };
	t.imagePanel = { 6, 31, 437, 327 };
	t.image = "MainImage";
	t.bar.rect = { 6, 362, 437, 22 };
	t.status = { 6, 388, 437, 0 };
	t.pause = { 373, 386, 70, 15 };
	t.buttonStyle.normal = "Button_n";
	t.buttonStyle.hover = "Button_h";
	t.buttonStyle.disabled = "Button_d";
	// Panel de botones en 96,412 + cada botón.
	t.options = { 110, 417, 67, 31 };
	t.play = { 191, 417, 67, 31 };
	t.close = { 272, 417, 67, 31 };
	t.credit = { 350, 442 };

	t.optionsBackground = "OptionsBackground";
	t.optionsTitlePos = { 6, 6 };
	t.optionsPanel = { 9, 33, 433, 364 };
	t.messageY = 402;
	t.save = { 150, 417, 67, 31 };
	t.back = { 233, 417, 67, 31 };
	return t;
}

} // namespace Launcher
