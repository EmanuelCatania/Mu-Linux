# MU-Guard

MU-Guard es la protección propia del cliente de Mu-Linux. Esta primera versión
cierra un solo camino: **abrir el juego sin pasar por su guardián**. Las
detecciones (editores de memoria, depuradores, inyección) llegan en las
próximas versiones.

## Cómo funciona

- El cliente de escritorio (`main.exe` en Windows, `main` en Linux) arranca y le
  pregunta al ConnectServer si el server tiene MU-Guard. Si lo tiene, ese
  proceso pasa a ser el **guardián**: saluda a `mu-guard` y abre el juego como
  un proceso hijo. Da igual cómo se haya abierto (launcher, acceso directo o
  doble clic).
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

## Activarlo

### Docker

En el `.env`:

```
MU_GUARD_ENABLED=1
MU_GUARD_REQUIRED=0
```

y abrir el puerto **44406/tcp** (`MU_GUARD_PORT`, ya publicado en `docker-compose.yml`).
Conviene arrancar con `MU_GUARD_REQUIRED=0`: los que entren sin guardián quedan
en el log como `Login sin guardian valido` y nadie queda afuera. Cuando todos
usan un cliente con MU-Guard, pasarlo a `1`.

### Pterodactyl

En el huevo del server: **MU-Guard** = 1, **Require MU-Guard** = 0 o 1, y
**MU-Guard port** con una asignación adicional del panel (el cliente la recibe
del ConnectServer, no hace falta que sea 44406).

### A mano

`Data/Guard/Guard.toml`:

```toml
Enabled = true
Required = false
Port = 44406
Socket = ""   # vacío = Data/Guard/mu-guard.sock
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
- Todavía no hay banner, ni protección de la memoria del juego, ni
  detecciones: un programa que se haga pasar por el guardián puede pedir
  tickets. Eso lo cierran las próximas versiones (desafío de memoria y builds
  aceptados).
