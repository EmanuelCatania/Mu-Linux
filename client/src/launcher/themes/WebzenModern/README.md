# Tema WebzenModern

Diseño del launcher "WebzenModern" de kayito (NicoMuratona), tomado de
https://github.com/nicomuratona/kayito-MuLauncher (carpeta `6.WebzenModern`).
Su autor dio permiso para usarlo en este proyecto (2026-11-20, por Discord); su
repo no tiene licencia, así que el crédito va acá y en la ventana del launcher.

Las imágenes son las del original, sin cambios; `MuIcon.png` sale de su
`Forms/Main.resx` (estaba embebido ahí) y `CheckBox_*.png` no se usan.
Medidas y colores de su `Forms/Main.Designer.cs` y `Forms/Options.Designer.cs`.
Se compila con `-DMU_LAUNCHER_THEME=WebzenModern`.

- Ventana de 1024x576 con la imagen (o la web del server, en Windows) casi de
  borde a borde.
- En la cabecera, el ícono y el título; el engranaje abre las opciones y la X
  cierra (no hay botón Cerrar).
- Abajo, el logo, el estado, las dos barras (como en el original, la de lo
  bajado usa `CurrentProgress` y la de archivos `CompleteProgress`) y Jugar.
- Las opciones eran otra ventana de 446x461: mientras están abiertas la ventana
  toma ese tamaño; se guardan con el botón verde y se cierran con la X.

`MainImage.png` es arte de MU Online (Webzen).
