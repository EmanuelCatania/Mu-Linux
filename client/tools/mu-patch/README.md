# mu-patch

Arma y verifica los canales de parches que va a usar el launcher. Todavía no
hay launcher que los consuma: esta es la primera pieza.

Un canal es una carpeta con:

- `manifest.json`: el estado final del canal (rutas, tamaños, hashes y dónde
  está cada archivo);
- `manifest.json.sig`: su firma;
- uno o más `.zip`.

Los zips usan zstd (método 93) y se abren con 7-Zip o con el `tar` de Windows 10
y 11. Cada archivo va comprimido por separado, así el launcher baja con HTTP
Range sólo los que le faltan.

## Canales

| Canal | Qué trae | Firma |
|---|---|---|
| `mu-linux/base` | `Content/` y `System/`, sin HD ni música | clave de Release del proyecto |
| `mu-linux/hd` | los `*-hd.*` | clave de Release del proyecto |
| `<server>/overlay` | el ejecutable del server y sus agregados | `Data/Keys/ServerIdentity.key` del server |

La base y el HD nunca traen ejecutables, porque cada server compila el suyo con
su dirección y su clave. Por eso el overlay siempre trae el del operador.

## Compilar

Sólo:

```bash
cmake -S client/tools/mu-patch -B build/mu-patch -DCMAKE_BUILD_TYPE=Release
```

```bash
cmake --build build/mu-patch --config Release
```

Desde el cliente: `cmake --build <build> --target mu-patch`.

## Uso

Clave de firma de un canal del proyecto:

```bash
mu-patch keygen --out release.key
```

Base:

```bash
mu-patch build --channel mu-linux/base --version 0.97.50 --in client/bin/Client --out dist/base --key release.key --include 'Content/**' --include 'System/**' --exclude '**/*-hd.*' --exclude 'Content/audio/music/**'
```

Overlay de un server. `requires.base` sale solo del ejecutable, de la marca
`MU_RELEASE`:

```bash
mu-patch build --channel miserver/overlay --version 1.0.0 --in mis-archivos --out update --key Data/Keys/ServerIdentity.key
```

Versión siguiente, que reusa los zips de la anterior y genera deltas:

```bash
mu-patch build ... --version 0.97.51 --out dist/base-0.97.51 --prev dist/base@https://ejemplo/releases/download/0.97.50
```

Verificar:

```bash
mu-patch verify --in dist/base --pubkey <hex> --prev dist/base-anterior
```

## Parches del operador: `publish` y `watch`

El admin no arma el canal a mano: deja cada parche en una carpeta
`Data/Patches/DDMMAAAA-N/` (fecha y número del día), con un `update.zip` armado
con cualquier herramienta (Deflate o sin comprimir) o con los archivos sueltos,
y opcionalmente un `deleted.txt` (una ruta por línea, para que el cliente borre
algo que agregó un parche anterior).

```bash
mu-patch publish --patches Data/Patches --channel miserver/overlay --key Data/Keys/ServerIdentity.key
```

- Las carpetas se ordenan por fecha (`19112026` va antes que `01122026`); un
  nombre que no cumple el formato se ignora con un aviso.
- El manifiesto es el **estado final**: para cada ruta gana el parche más nuevo,
  y `deleted.txt` la saca (un parche posterior la puede volver a traer).
- Sale en `Data/Patches/published/` (es lo único que se sirve): manifiesto
  firmado, zips y `history/` con las últimas 3 versiones.  Lo que no cambió se
  reusa de los zips ya publicados; lo nuevo va en zips nuevos, con delta contra
  esas versiones.  Un zip que ya no usa ninguna de las 3 se borra.
- Si nada cambió no publica una versión nueva.  La versión es `1.0.<secuencia>`.

`watch` hace lo mismo en un bucle (es el proceso del server con
`MU_UPDATE_ENABLED=1`): publica cuando `Data/Patches` cambia y queda igual entre
dos pasadas, así una carpeta a medio subir no se publica.

```bash
mu-patch watch --patches Data/Patches --channel miserver/overlay --key Data/Keys/ServerIdentity.key --interval 30
```

### Opciones de `build`

| Opción | |
|---|---|
| `--include`, `--exclude` | globs sobre las rutas con `/`: `*` y `?` no cruzan carpetas, `**` sí. |
| `--optional` | archivos que no hacen falta para empezar a jugar; el launcher los baja después. |
| `--prev <carpeta>[@<url>]` | versión anterior del mismo canal. Los archivos que no cambiaron apuntan a sus zips. Si se da la URL, los zips quedan allá; si no, se copian a la salida. Los que cambiaron llevan un delta contra las 3 versiones más nuevas, sólo si el archivo pesa al menos 1 MB y el delta ahorra 30 % o más. |
| `--deleted <archivo>` | una ruta por línea, para que el cliente las borre. |
| `--base-manifest` | overlay: avisa qué archivos de la base pisa. |
| `--sequence` | número de secuencia. Por defecto, el de la versión anterior + 1. El launcher no acepta uno menor que el que ya tiene, así nadie puede reenviar un parche viejo con firma válida. |
| `--bundle-size` | tamaño máximo de cada zip, en MB (por defecto 64). |
| `--level` | nivel de zstd (por defecto 19). |

Se rechazan:

- rutas con `..`, absolutas o dentro de `User/` (los datos del jugador);
- ejecutables en la base o en el HD;
- `.toml` en el overlay, porque esos los manda el server en el handshake;
- un overlay sin ejecutable;
- una carpeta de salida que no esté vacía.

## Releases del proyecto

El workflow `Client channels` arma la base, la música y el HD en cada Release y
los adjunta a la Release (script: `.github/scripts/client_channels.py`). Los
assets son `mu-linux-base.manifest.json` (+ `.sig`), `mu-linux-music.*` y
`mu-linux-hd.*`, más sus zips. Los zips sin `url` están en la misma Release que
el manifiesto.

- Reusa los zips de las 3 Releases anteriores para lo que no cambió y baja sólo
  los que hacen falta para los deltas.
- La secuencia es la más alta publicada en el canal + 1, así un parche publicado
  después (una 0.97.47 tras la 0.97.50) no queda por debajo.
- Firma con el secreto `MU_RELEASE_SIGNING_KEY`: el contenido de un archivo de
  `mu-patch keygen`. Sin el secreto avisa y no publica. Guardá una copia de la
  clave fuera de GitHub: el secreto no se puede volver a leer, y sin la clave
  las Releases siguientes no pueden reusar ni firmar sobre las anteriores.
- En un PR que toca `mu-patch` corre en seco, con una clave descartable.

## Versión del cliente

El ejecutable lleva compilada su versión: `MU_RELEASE`, en
`src/app/config/Release.h`, que se actualiza en cada Release. Un build de CMake
la puede fijar con `-DMU_RELEASE=<versión>`.
No es `ClientVersion`, que es la que compara el server en el login.

Para ver la de un ejecutable:

```bash
mu-patch release-of main.exe
```

## El launcher en el overlay

El launcher compilado por el operador (`client/src/launcher`) va en la carpeta
`Launcher/` del overlay: `Launcher/launcher.exe` y, para Linux,
`Launcher/launcher`. `mu-patch` marca `"self": true` a los ejecutables de esa
carpeta, y el launcher los trata distinto:

- se instalan antes que todo lo demás; el launcher se reinicia y la versión
  nueva baja el resto;
- el que está corriendo no se pisa: queda como `launcher.exe.old` hasta que el
  nuevo anda unos segundos;
- si el nuevo no llega a confirmar, el siguiente arranque vuelve al anterior y
  no reinstala esa versión hasta que se publique otra.

Detalle en `client/src/launcher/SelfUpdate.h`.

## Pendiente

- Correr `watch` en la imagen del server y servir `published/` desde `mu-web`.
