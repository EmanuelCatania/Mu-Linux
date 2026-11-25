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
- El GameServer le pregunta a `mu-guard` si el ticket es válido. Con
  `MU_GUARD_REQUIRED=1`, un login sin ticket válido se rechaza con el mensaje
  "La protección MU-Guard no está activa".
- Si `mu-guard` se cae, el GameServer lo nota solo (latido cada segundo) y deja
  entrar sin validar hasta que vuelva, avisándolo en el log. Los que ya están
  jugando siguen.

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
| `INJECT` | otros módulos fuera del juego y del sistema | sólo se registra |

Al cerrar, el guardián muestra el banner rojo de MU-Guard con lo que detectó y
una cuenta regresiva. Todo queda en el log de `mu-guard` con la IP del
jugador. Los módulos desconocidos sólo se registran hasta tener las listas por
server.

Editar valores de la memoria (vida, zen...) no se puede ver de forma confiable
desde afuera, porque el juego los cambia todo el tiempo. La defensa es que un
editor no llegue a encontrarlos (`MEMSCAN` cierra el juego en su primer
escaneo) y que esos valores los decida el server.

Si un antivirus lee la memoria del juego puede dar `MEMSCAN` a un jugador
limpio. Si pasa, en `Data/Guard/Guard.toml`:

```toml
MemScanCloses = false   # MEMSCAN sólo se registra
```

Se aplica a los guardianes que se conecten después del cambio.

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

## Activarlo

### Docker

En el `.env`:

```
MU_GUARD_ENABLED=1
MU_GUARD_REQUIRED=0
```

y abrir el puerto **28980/tcp** (`MU_GUARD_PORT`, ya publicado en `docker-compose.yml`).
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
- Con administrador se saltea la DACL. Para eso están las detecciones y el
  desafío de memoria. Un programa que se haga pasar por el guardián tiene que
  contestar los desafíos con el código de un `main.exe` sin tocar: eso lo deja
  afuera si modifica el código del juego, pero no si edita sólo datos.
- En Linux con `ptrace_scope` 0 no hay bloqueo de lectura de la memoria del
  juego, sólo la detección que llega después.
