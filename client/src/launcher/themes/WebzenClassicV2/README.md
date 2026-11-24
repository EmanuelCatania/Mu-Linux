# Tema WebzenClassicV2

Diseño del launcher "WebzenClassicV2" de kayito (NicoMuratona), tomado de
https://github.com/nicomuratona/kayito-MuLauncher (carpeta `5.WebzenClassicV2`).
Su autor dio permiso para usarlo en este proyecto (2026-11-20, por Discord); su
repo no tiene licencia, así que el crédito va acá y en la ventana del launcher.

Las imágenes son las del original, sin cambios.  Medidas y colores de su
`Forms/Main.Designer.cs` y `Forms/Options.Designer.cs`; paleta dorada (Goldenrod / DarkGoldenrod).
Se compila con `-DMU_LAUNCHER_THEME=WebzenClassicV2`.

- Ventana de 792x582 con la forma del fondo: lo transparente de
  `MainBackground.png` no se dibuja ni recibe clics (el `TransparencyKey`
  del original).  En Windows es una región sobre la ventana; en el resto de los
  sistemas, `SDL_SetWindowShape`.
- Se arrastra desde la franja del `Header_panel` (debajo del caballero) y se
  cierra con la X.
- Dos barras con imagen: TOTAL es lo bajado y UPDATE, los archivos.
- El panel central muestra la web del server (en Windows) o, si no hay, el
  fondo con las noticias.
- Las opciones eran otra ventana: mientras están abiertas la ventana toma su
  tamaño (385x451) y su fondo, con las pestañas de a tres por fila.
