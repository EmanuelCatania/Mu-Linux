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

## Lo que el launcher aprende del server

Con `MU_UPDATE_ENABLED=1` el server publica además un `launcher.json` firmado
con su identidad. El launcher lo pide al ConnectServer cada vez que arranca y lo
guarda: así podés cambiar la dirección del ConnectServer o la de los parches sin
volver a repartir el launcher.

En `.env`:

```bash
MU_UPDATE_URL=https://mu.ejemplo.com/updates
MU_LAUNCHER_CONNECT_SERVERS=mu.ejemplo.com:44405,respaldo.ejemplo.com:44405
MU_LAUNCHER_NEWS=Mantenimiento el lunes a las 10.\nDura una hora.
```

- `MU_UPDATE_URL`: la dirección pública de `/updates/` (la de tu web, como la ve
  un jugador). Podés poner varias separadas por coma: el launcher prueba la
  siguiente si una falla. Vacía, el launcher sigue usando la que tiene compilada.
- `MU_LAUNCHER_CONNECT_SERVERS`: las direcciones del ConnectServer
  (`host:puerto`). Vacía, se usa `PUBLIC_IP:44405`.
- `MU_LAUNCHER_NEWS`: un texto que el launcher muestra sobre la imagen. `\n`
  corta la línea.
- `MU_LAUNCHER_WEB_URL`: el sitio del server (`https://...`). En Windows el
  launcher lo muestra en el panel de la imagen apenas carga (los enlaces a otros
  sitios se abren en el navegador), y en cualquier sistema lo abre el botón
  "Ir a la web" de Opciones > General.
- `MU_LAUNCHER_BASE_SOURCE`, `MU_LAUNCHER_BASE_TAG`, `MU_LAUNCHER_BASE_KEY`: el
  origen del cliente base, si usás un fork propio (vacíos: el compilado en el
  launcher).

### Jugar sólo desde el launcher

`MU_LAUNCHER_REQUIRED=1` (o `RequireLauncher=1` en la sección `[Launcher]` de
`ConnectServer.ini`, fuera de docker) hace que el cliente de escritorio abierto
directo, sin el launcher, abra el launcher y se cierre, como el `main.exe` del
0.97k original con `mu.exe`. Así nadie juega sin actualizar. El cliente lo
pregunta al ConnectServer en la intro, por el canal cifrado; el `.ini` se lee en
cada pedido, así que cambiarlo no pide reiniciar.

- El launcher tiene que estar al lado del juego: en `Launcher/` (donde lo deja
  su parche) o en la misma carpeta. Si no está, el cliente avisa que hay que
  bajarlo del sitio del server y se cierra.
- Sólo aplica al cliente de **escritorio** (Windows y Linux). La versión web no
  tiene launcher: no lo pregunta ni lo aplica.
- No es seguridad: el launcher marca el juego con una variable de entorno
  (`MU_STARTED_BY_LAUNCHER`) que cualquiera puede poner. Lo que protege es la
  seguridad del server.

**Para mudar el ConnectServer**: agregá la dirección nueva en
`MU_LAUNCHER_CONNECT_SERVERS` **antes** de apagar la vieja y dejala unos días.
Cada launcher que se conecta la aprende; uno que ya no encuentra la vieja prueba
las demás y, como último recurso, baja `launcher.json` de `/updates/`.

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
- Mientras `Data/Patches/` no tenga ningún parche, el server publica una versión
  vacía: los launchers ven que no hay nada para bajar y dejan jugar. Lo mismo si
  borrás todos los parches: los jugadores vuelven a los archivos de la base.
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

## Pterodactyl

Los eggs (`Pterodactyl eggs/`) traen las mismas opciones, como variables de
Startup:

- **Server**: `MU_UPDATE_ENABLED`, `MU_UPDATE_CHANNEL`, `MU_UPDATE_INTERVAL`,
  `MU_UPDATE_URL`, `MU_LAUNCHER_CONNECT_SERVERS`, `MU_LAUNCHER_NEWS`,
  `MU_LAUNCHER_WEB_URL`, `MU_LAUNCHER_BASE_SOURCE`, `MU_LAUNCHER_BASE_TAG`,
  `MU_LAUNCHER_BASE_KEY` y `MU_LAUNCHER_REQUIRED`.
- **Web**: `MU_UPDATE_ENABLED` (en 1 sirve `/updates/`), `UPDATES_DIR` (dónde
  están los parches publicados) y
  `UPDATES_RATE_LIMIT_PER_MINUTE`.

Los parches se suben igual, con el administrador de archivos del panel, a
`Data/Patches/DDMMAAAA-N/`.

La diferencia con docker es que el server y la web son dos servers de
Pterodactyl y no comparten archivos. Para que la web sirva lo que publica el
server hace falta un **Mount**, y lo arma quien administra el panel:

1. En el nodo, una carpeta del host para los parches publicados, de la que el
   contenedor sea dueño (corre con el usuario 988) y cuyas carpetas de arriba
   no lo sean ni las pueda escribir cualquiera (Wings rechaza el mount si no):

   ```bash
   sudo mkdir -p /srv/mu-updates
   sudo chown 988:988 /srv/mu-updates
   ```

2. Agregarla a `allowed_mounts` en `/etc/pterodactyl/config.yml` y reiniciar
   Wings:

   ```yaml
   allowed_mounts:
     - /srv/mu-updates
   ```

3. En el panel, Admin > Mounts > Create: origen `/srv/mu-updates`, destino
   `/home/container/Data/Patches/published`, sin "Read only". Un mount tiene un
   solo destino para todos los servers donde se use: por eso la web lo ve en la
   misma ruta, que es el valor por defecto de su `UPDATES_DIR`.
4. En el mismo mount, agregar los dos eggs (server y web) y el nodo. Después, en
   cada uno de los dos servers, Admin > Servers > Mounts y agregarlo.
5. Reiniciar los dos.

Sólo va `published/` en el mount, no `Data/Patches` entero: el administrador de
archivos trabaja sobre el volumen del server y no ve lo que hay montado encima,
así que las carpetas de los parches tienen que quedar en el volumen para poder
subirlas.

Sin el mount, todo lo demás funciona: el launcher recibe su configuración por el
ConnectServer y "jugar sólo desde el launcher" no depende de la web. Los parches
se publican igual en `Data/Patches/published`; para repartirlos hay que copiar
esa carpeta a otro lado (un hosting, un CDN) y poner esa dirección en
`MU_UPDATE_URL`.
