# Tema WebzenOriginal

Diseño del launcher "WebzenOriginal" de kayito (NicoMuratona), tomado de
https://github.com/nicomuratona/kayito-MuLauncher (carpeta `1.WebzenOriginal`).
Su autor dio permiso para usarlo en este proyecto (2026-11-20, por Discord); su
repo no tiene licencia, así que el crédito va acá y en la ventana del launcher.

El launcher con controles de Windows: ventana de 294x386 con la barra de título
del sistema, el banner estirado arriba (como en el original), barra de progreso
en bloques y botones grises.  Medidas de su `Forms/Main.Designer.cs`.  Se
compila con `-DMU_LAUNCHER_THEME=WebzenOriginal`.

Diferencias con el original:
- Ahí las opciones estaban en la misma ventana (cuatro recuadros).  Acá hay más
  opciones de las que entran: en ese lugar va el banner con las noticias del
  server, se suma el botón Opciones entre Play y Close, y mientras las opciones
  están abiertas la ventana crece a 450x420.
- No muestra la web del server en la ventana (el original tampoco): tiene el
  botón Web.

`Banner.png` es arte de MU Online (Webzen).
