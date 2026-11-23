# Parches del launcher

El launcher del jugador baja el cliente base de los Releases del proyecto y,
encima, **los archivos de tu server**: tu ejecutable (que lleva la dirección y la
clave de tu server), tu launcher y lo que agregues (modelos, texturas, mapas).
Eso último lo publica el propio server desde `Data/Patches/`.

## Activarlo

En `.env`:

```bash
MU_UPDATE_ENABLED=1
MU_UPDATE_CHANNEL=miserver/overlay
```

- `MU_UPDATE_CHANNEL` es el nombre de tu canal: `<algo>/overlay`, en minúsculas,
  sin espacios. Tiene que coincidir con el que compilaste en tu launcher
  (`-DMU_OVERLAY_CHANNEL`).
- `MU_UPDATE_INTERVAL` (por defecto 30): cada cuántos segundos se mira si
  cambió algo.
- `UPDATES_RATE_LIMIT_PER_MINUTE` (por defecto 120): pedidos por minuto y por IP
  a `/updates/` en `mu-web`.

Con eso `mu-server` publica los parches y `mu-web` los sirve en
`http://<tu web>/updates/`. Esa es la URL que va en tu launcher
(`-DMU_OVERLAY_URL`). Si tenés un proxy o un CDN adelante, `/updates/` no
necesita nada especial: los zips se pueden guardar en caché para siempre (su
nombre cambia con cada versión) y el manifiesto no.

## Subir un parche

Cada parche es una carpeta en el volumen `mu_patches` (dentro del contenedor,
`/opt/mu/Data/Patches/`), con nombre **`DDMMAAAA-N`**: la fecha y el número de
parche de ese día.

```
Data/Patches/
├── 19112026-1/
│   └── update.zip          ← un zip armado con cualquier programa
├── 19112026-2/
│   ├── main.exe            ← o los archivos sueltos, con la estructura del cliente
│   ├── Launcher/launcher.exe
│   ├── Content/item/Sword01.ozj
│   └── deleted.txt         ← opcional: rutas que el cliente tiene que borrar
└── published/              ← lo genera el server: no tocar
```

Por ejemplo, para copiar una carpeta desde tu máquina:

```bash
docker compose cp ./19112026-2 mu-server:/opt/mu/Data/Patches/
```

- **El primer parche tiene que traer tu ejecutable** (`main.exe` para Windows,
  el binario para Linux). Un parche posterior puede traer sólo lo que cambió.
- Tu launcher va en `Launcher/`. El launcher se actualiza a sí mismo antes que
  todo lo demás.
- Las carpetas se aplican **por fecha**: para cada archivo vale la versión del
  parche más nuevo. Un jugador que se salteó cinco parches baja sólo la última
  versión de cada archivo.
- `deleted.txt`: una ruta por línea (por ejemplo `Content/item/Viejo.ozj`), para
  que el cliente borre algo que agregó un parche anterior o vuelva al de la base.
- Los `.toml` de `Content` no van en un parche: los manda el server al conectar
  (`Data/ClientContent`).
- Un nombre que no cumple el formato se ignora y se avisa en el log.

A los pocos segundos de que la carpeta deja de cambiar, el server publica una
versión nueva (si el contenido cambió). Una carpeta a medio subir no se publica:
espera a que se quede quieta. El log de `mu-server` lo muestra:

```
publicada la 1.0.3 (secuencia 3)
```

Para publicar a mano o ver qué pasa:

```bash
docker compose exec mu-server mu-patch publish --patches /opt/mu/Data/Patches \
  --channel miserver/overlay --key /opt/mu/Data/Keys/ServerIdentity.key
```

## Qué queda en `published/`

El manifiesto firmado con la identidad del server
([identidad-del-server.md](identidad-del-server.md)), los zips y `history/` con
las últimas 3 versiones. Lo que no cambió se sigue sirviendo de los zips de
antes; los archivos grandes que cambiaron llevan además un parche binario
contra esas versiones. Un zip que ya no usa ninguna de ellas se borra solo.

`mu-web` sirve sólo el manifiesto, su firma y los zips de esa carpeta: nunca una
carpeta de parche, ni `history/`, ni los datos del server. El volumen se monta de
sólo lectura.
