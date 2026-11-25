# MU-Guard

MU-Guard es la protección propia del cliente de Mu-Linux: que el juego sólo se
abra a través de su guardián, que ningún otro programa lea o edite su memoria y
que su código no esté modificado.

## Cómo funciona

- El cliente de escritorio (`main.exe` en Windows, `main` en Linux) arranca y le
  pregunta al ConnectServer si el server tiene MU-Guard. Si lo tiene, ese
  proceso pasa a ser el **guardián**: saluda a `mu-guard` y abre el juego como
  un proceso hijo. Da igual cómo se haya abierto (launcher, acceso directo o
  doble clic).
- Mientras tanto el guardián muestra el banner de MU-Guard con lo que va
  verificando. Se va cuando aparece la ventana del juego.
- En Windows, el guardián y el juego quedan con una lista de permisos (DACL) que
  no deja a ningún otro programa del mismo usuario leer ni escribir su memoria,
  duplicar sus handles o cambiarles los permisos. Terminarlos sí (el
  Administrador de tareas sigue funcionando). En Linux el guardián no se puede
  leer y el juego lo protege Yama (`ptrace_scope` 1: sólo un proceso padre).
- En cada intento de login el juego le pide al guardián un **ticket** de un
  solo uso. `mu-guard` lo ata a la IP del jugador, vence en 60 segundos y sólo
  sirve en la conexión donde se presenta.
  Excepción: si el login llega desde la red del server (IP privada), se acepta
  también un ticket pedido desde otra IP privada o desde la IP pública del
  server (`PUBLIC_IP`). Es el caso de jugar desde la misma red entrando por el
  dominio: según el puerto, el router deja pasar la IP privada o pone la
  pública.
- El GameServer le pregunta a `mu-guard` si el ticket es válido. Con
  `MU_GUARD_REQUIRED=1`, un login sin ticket válido se rechaza con el mensaje
  "La protección MU-Guard no está activa".
- Si `mu-guard` se cae, el GameServer lo nota solo (latido cada segundo) y deja
  entrar sin validar hasta que vuelva, avisándolo en el log. Los que ya están
  jugando siguen. Con `MU_GUARD_REQUIRED=1`, los que entraron mientras estaba
  caído tienen 60 segundos desde que vuelve para validar: el juego le pide un
  ticket nuevo a su guardián y lo manda solo. El que no valida queda
  desconectado.
- El GameServer le avisa a cada jugador el estado de `mu-guard` cada 5
  segundos, dentro del canal cifrado y siempre con el mismo tamaño: desde afuera
  no se ve cuándo hay una caída. El juego no baja la guardia por el aviso.

Todo viaja por el mismo canal cifrado que el juego y con la misma identidad del
server (`Data/Keys/ServerIdentity.key`).

## Detecciones

El guardián revisa el juego cada 2 segundos desde afuera, con prioridad baja.

| Detección | Qué ve | Qué hace |
|---|---|---|
| `DEBUGGER` | un depurador sobre el juego o el guardián | cierra el juego |
| `MEMEDIT` | el driver de Cheat Engine (Windows) u otro programa con la memoria del juego abierta (Linux) | cierra el juego |
| `INJECT` | módulos de Cheat Engine, o las funciones de tiempo del sistema reescritas dentro del juego (speedhack) | cierra el juego |
| `MEMSCAN` | alguien leyó las páginas señuelo que el guardián deja en el juego (el escaneo de un editor de memoria) | cierra el juego (`MemScanCloses`) |
| `TOOL` | una herramienta de `Data/Guard/Lists.toml` abierta (proceso o ventana) | cierra el juego (`ToolCloses`) |
| `FILES` | un archivo del juego distinto del de `Data/Guard/Files.txt`, o que falta | cierra el juego (`FilesCloses`) |
| `INJECT` | un módulo de la lista `block` de `Lists.toml` | cierra el juego |
| `INJECT` | otros módulos fuera del juego y del sistema | sólo se registra (salvo los de `allow` y los firmados por Microsoft) |
| `KILLED` | un detector del guardián dejó de correr mientras el guardián sigue contestando el desafío | cierra el juego |

Al cerrar, el guardián muestra el banner rojo de MU-Guard con lo que detectó y
una cuenta regresiva. Todo queda en el log de `mu-guard` con la IP del
jugador. Los módulos desconocidos sólo se registran hasta tener las listas por
server.

Editar valores de la memoria (vida, zen...) no se puede ver de forma confiable
desde afuera, porque el juego los cambia todo el tiempo. La defensa es que un
editor no llegue a encontrarlos (`MEMSCAN` cierra el juego en su primer
escaneo) y que esos valores los decida el server.

Un antivirus que lee la memoria del juego puede dar `MEMSCAN` a un jugador
limpio. Por eso `MEMSCAN` cierra el juego pero **no suma aviso** salvo que se
agregue a `[warnings] codes`: en el peor caso el antivirus le cierra el juego,
nunca lo banea. Si en el log aparecen `MEMSCAN` de jugadores que no deberían, en
`Data/Guard/Guard.toml`:

```toml
MemScanCloses = false   # MEMSCAN sólo se registra
```

Se aplica a los guardianes que se conecten después del cambio.

## Listas y textos

Las listas vienen en dos archivos que `mu-guard` suma:

- `Data/Guard/Lists.default.toml`, las de fábrica. **No se edita**: el arranque
  lo reescribe en cada versión, así un server ya instalado recibe las nuevas.
- `Data/Guard/Lists.toml`, las tuyas. Si no está, el arranque lo crea vacío y
  después no lo toca.

`mu-guard` se las manda al guardián al conectarse y otra vez cuando cambia
cualquiera de los dos, sin reiniciar nada. El formato es el mismo:

```toml
[tool]
# Nombre del ejecutable, sin importar mayúsculas (el guardián los revisa cada 10 s).
processes = ["cheatengine-x86_64.exe", "artmoney.exe", "x64dbg.exe"]
# Parte del título de una ventana.
windows = []

[modules]
# Parte del nombre de un módulo cargado en el juego: lo cierra.
block = []
# Parte de la ruta de un módulo que no hace falta registrar (overlays, antivirus).
allow = [
  "obs-studio-hook", "discord", "gameoverlayrenderer",
  # Capa de Microsoft para OpenGL/Vulkan (tienda de Windows).
  "microsoft.d3dmappinglayers",
  # Antivirus que meten su DLL en los programas.
  'windows defender', 'avast software', 'avg\antivirus', 'bitdefender', 'kaspersky lab',
  '\eset\', 'malwarebytes', 'norton', 'mcafee', 'trend micro', 'sophos', 'panda security',
  'f-secure', 'webroot', 'comodo', 'avira', '360total security', '360safe',
]
```

La lista de fábrica trae Cheat Engine, ArtMoney, x64dbg, OllyDbg, WPE Pro,
scanmem, GameConqueror y PINCE. Con `ToolCloses = false` en `Guard.toml` las
herramientas sólo se registran. Una herramienta abierta no suma aviso salvo que
se agregue `"TOOL"` a `[warnings] codes`. Cuidado con `windows`: "cheat engine"
también está en el título del navegador con esa página abierta.

`Data/Guard/Messages.toml` cambia cómo se nombra cada detección en el banner
("Se detectó *un programa no permitido*"), por idioma:

```toml
[es]
TOOL = "un programa no permitido"
[en]
TOOL = "A forbidden program"
[pt]
TOOL = "Um programa proibido"
```

Sin una entrada se usa el texto del cliente. Si los archivos no están, el
entrypoint (y el start de Pterodactyl) los crea con lo de fábrica.

## Desafío de memoria y builds aceptados

Cada unos 30 segundos `mu-guard` le pide al guardián el hash de 8 bloques de
64 KB del código del juego, tal como está cargado en memoria, con una clave
nueva cada vez. Los bloques salen de una recorrida en orden aleatorio de todo
el código: en unos 16 minutos (para un `main.exe` de 16 MB de código) se
revisó entero, y vuelve a empezar en otro orden. Lo compara con el hash que
calcula él mismo a partir del `main.exe` publicado:

- si no coincide (el código del juego fue modificado), le ordena al guardián
  cerrar el juego (banner rojo `PATCH`) y desconecta del GameServer a los
  jugadores que entraron con ese guardián;
- si el guardián no contesta en 20 segundos, o se desconecta y no vuelve en 30,
  también los desconecta.

Para eso `mu-guard` necesita el **manifiesto** de cada `main.exe` que reparte
el server, en `Data/Guard/Builds/`. Las Releases no traen ejecutables (cada
server compila el suyo), así que el manifiesto lo arma quien compila:

```sh
# en Docker, con el main.exe copiado a la carpeta Data del server
docker compose exec mu-server /opt/mu/mu-guard build     /opt/mu/Data/main.exe /opt/mu/Data/Guard/Builds/main.exe.guardbuild
```

Sirve igual para el `main` de Linux. Se puede tener uno por cada versión del
cliente que todavía se use; `mu-guard` relee la carpeta cada 10 segundos.

Un cliente que no está en `Data/Guard/Builds` entra sin desafío de memoria (y
queda en el log como `sin build registrado`). Para aceptar sólo los builds
publicados:

```toml
AcceptUnknownBuilds = false
```

El cliente que no está en la lista no consigue tickets, así que con
`MU_GUARD_REQUIRED=1` no entra.

El intervalo se ajusta con `ChallengeSeconds` (30 por defecto, de 5 a 300): más
corto revisa todo el código antes y le cuesta más CPU a `mu-guard` (hashear
512 KB por jugador en cada desafío).

Cada respuesta trae además cuántas veces corrió cada detector del guardián.
Si entre una respuesta y la siguiente las pasadas no avanzan, o un detector que
ya corría se queda quieto mientras las pasadas siguen, `mu-guard` cierra el
juego con `KILLED`. Sólo funciona con builds registrados (sin build no hay
desafío).

## Archivos del cliente (FILES)

`mu-guard` puede verificar que los datos del juego sean los de tu cliente: los
`.bmd`, `.toml` y `.att` de `Content/` (items, skills, terreno, modelos), que son
los que dan ventaja si se tocan. Las texturas y el sonido no se verifican: son
muchos y el pack HD los reemplaza.

`Files.txt` es la lista de la **base** del cliente: se arma una vez, desde el
cliente que repartís, y otra vez cuando cambia la versión base. Los parches no
hace falta: `mu-guard` le suma lo que trae el overlay publicado por `mu-patch`
(`Data/Patches/published/manifest.json`), lo que agrega o cambia y lo que
borra, y lo relee solo cuando publicás uno nuevo.

```sh
# en Docker, con la carpeta del cliente copiada a Data/cliente
docker compose exec mu-server /opt/mu/mu-guard files /opt/mu/Data/cliente /opt/mu/Data/Guard/Files.txt
```

Sin `Files.txt` no se verifica nada. `mu-guard` lo relee solo y se lo manda a
los guardianes conectados. El guardián lo revisa de a poco (unos 4 MB cada 2
segundos: un cliente entero, en unos 30 segundos) y otra vez cada 10 minutos.
Un archivo distinto, o que falta, cierra el juego; con `FilesCloses = false` en
`Guard.toml` sólo se registra. No suma aviso salvo que se agregue `"FILES"` a
`[warnings] codes`: un parche a medio bajar también da `FILES`.

## Avisos y bans

Cada detección que cierra el juego (y cada desafío de memoria fallado) suma un
**aviso**: uno por arranque del guardián, aunque se detecte varias veces. Al
llegar a `limit` avisos dentro de `window_days`, se banea:

- **la cuenta**: con el bloqueo de siempre (`MEMB_INFO.bloc_code`), así que el
  JoinServer lo levanta solo al vencer y el GameServer la rechaza con "cuenta
  bloqueada";
- **la máquina**: un hash de la PC que calcula el guardián (UUID del SMBIOS,
  serie del disco del sistema y MAC en Windows; `machine-id` en Linux).
  `mu-guard` lo guarda mezclado con una clave propia del server
  (`Data/Guard/MachineSalt.key`), así no sirve para cruzar jugadores entre
  servers. Una máquina baneada ve el banner "ACCESO BLOQUEADO" y el juego no
  se abre;
- **la IP** de ese último aviso, sólo junto con el ban de cuenta o de máquina.
  Nunca se banean IP privadas, loopback ni CGNAT: detrás de un proxy o de
  algunos Docker todos llegan con la misma. Una IP baneada no pasa el saludo y
  tampoco entra sin ticket aunque no se exija.

La cuenta cuenta sólo si la detección fue con la sesión iniciada (la informa el
GameServer, nunca el cliente); la máquina, siempre. Cuando un ban vence, sus
avisos anteriores dejan de contar. El banner rojo muestra qué aviso fue ("Aviso
2 de 3") o hasta cuándo quedó bloqueado. Perder el contacto con el guardián
desconecta pero **no** suma aviso: un corte de internet no banea a nadie.

`Data/Guard/Guard.toml`:

```toml
[warnings]
limit = 3              # 0 = nunca banear
window_days = 30
codes = ["DEBUGGER", "INJECT", "MEMEDIT", "PATCH", "KILLED"]

[ban]
account_days = 7       # 0 = permanente, -1 = no banear la cuenta
machine_days = 7
ip_hours = 24
exempt_accounts = []   # staff: se registran pero no se banean
exempt_ips = []
```

Todo queda en la base, en dos tablas que crea `mu-guard` al arrancar:

- `MuGuardWarning`: fecha, detección, detalle, cuenta, personaje, máquina, IP,
  build, número de aviso y si se anuló (`voided`);
- `MuGuardBan`: tipo (`account`, `machine`, `ip`), valor, motivo, vencimiento
  (`NULL` = permanente) y si se levantó (`lifted`).

Y en el log de `mu-guard`, una línea por aviso:

```
[MU-Guard] MEMSCAN cuenta=... pj=... ip=... maquina=... aviso=2/3 -> BANEADO
```

`mu-guard` se conecta con los datos de `JoinServer/JoinServer.ini` (junto a su
ejecutable, o el de `MU_GUARD_DB_INI`); `DB_HOST`, `DB_PORT`, `DB_USER`,
`DB_PASS` y `DB_NAME` los pisan. Sin base sigue funcionando: la detección
cierra igual el juego y el aviso queda sólo en el log.

Para levantar un ban a mano (hasta que esté el panel de `mu-web`): poner
`lifted = 1` en `MuGuardBan` y, para la cuenta, `bloc_code = 0` en `MEMB_INFO`.
`mu-guard` relee los bans cada 30 segundos.

## Activarlo

### Docker

En el `.env`:

```
MU_GUARD_ENABLED=1
MU_GUARD_REQUIRED=0
```

y abrir el puerto **28980/tcp** (`MU_GUARD_PORT`, ya publicado en `docker-compose.yml`).
Si tu firewall filtra lo que Docker publica (reglas en la cadena `DOCKER-USER`),
el 28980 va al lado del 44405 y el 55901: `ufw allow` solo no alcanza. Con el
puerto cerrado el guardián no llega a `mu-guard`, el juego abre sin protección y
el login se rechaza.
Conviene arrancar con `MU_GUARD_REQUIRED=0`: los que entren sin guardián quedan
en el log como `Login sin guardian valido` y nadie queda afuera. Cuando todos
usan un cliente con MU-Guard, pasarlo a `1`.

### Pterodactyl

En el huevo del server: **MU-Guard** = 1, **Require MU-Guard** = 0 o 1, y
**MU-Guard port** con una asignación adicional del panel (el cliente la recibe
del ConnectServer, no hace falta que sea 28980).

### A mano

`Data/Guard/Guard.toml`:

```toml
Enabled = true
Required = false
Port = 28980
Socket = ""   # vacío = Data/Guard/mu-guard.sock
MemScanCloses = true
AcceptUnknownBuilds = true
ChallengeSeconds = 30
ToolCloses = true
MaxPerMachine = 0   # juegos abiertos a la vez en una misma PC (0 = sin límite)
FilesCloses = true  # un archivo distinto de Files.txt cierra el juego

[warnings]
limit = 3
window_days = 30
codes = ["DEBUGGER", "INJECT", "MEMEDIT", "PATCH", "KILLED"]

[ban]
account_days = 7
machine_days = 7
ip_hours = 24
exempt_accounts = []
exempt_ips = []
```

El ConnectServer y el GameServer lo releen cuando cambia, sin reiniciar. Hay que
lanzar `mu-guard` desde la carpeta del server (lee `Data/` igual que el
GameServer, o la de `MU_DATA_PATH`).

## Compilar sin MU-Guard

Cliente y server tienen la opción de CMake `MU_GUARD` (`ON` por defecto). Con
`-DMU_GUARD=OFF` no se compila nada de MU-Guard: el cliente no pregunta y el
server se comporta como antes.

## Límites de esta versión

- `mu-guard` y la verificación en el GameServer son sólo Linux (Docker y
  Pterodactyl).
- La versión web no tiene guardián: con `MU_GUARD_REQUIRED=1` no puede entrar.
- El hash de la máquina se puede falsificar: el ban de máquina le sube el costo
  a quien vuelve, el firme es el de cuenta.
- Si el guardián no llega a `mu-guard` cuando detecta algo, cierra el juego
  igual y guarda la detección en `User/Cache/guard-pending.txt`; se manda en el
  próximo arranque y suma el aviso a la máquina (la cuenta de esa sesión no se
  sabe). Quien borre ese archivo antes lo evita.
- Quien mata al guardián antes de que llegue el reporte evita el aviso (pero no
  sigue jugando).
- Las herramientas se reconocen por el nombre del ejecutable: una renombrada no
  cae en `TOOL`, pero sigue cayendo en las detecciones que miran lo que hace
  (depurador, driver, escaneo de memoria, código modificado).
- Los hilos que el juego crea después de arrancar reciben la DACL en la
  siguiente pasada del guardián: hay una ventana de hasta 2 segundos.
- Un cliente compilado con MU-Guard y sin `ServerPublicKey` (la clave del
  server, `app/config/ServerConfig.h`) sirve para probar, pero `mu-patch` no lo
  publica: sin la clave un proxy se hace pasar por el server.
- Con administrador se saltea la DACL. Para eso están las detecciones y el
  desafío de memoria. Un programa que se haga pasar por el guardián tiene que
  contestar los desafíos con el código de un `main.exe` sin tocar: eso lo deja
  afuera si modifica el código del juego, pero no si edita sólo datos.
- En Linux con `ptrace_scope` 0 no hay bloqueo de lectura de la memoria del
  juego, sólo la detección que llega después.
