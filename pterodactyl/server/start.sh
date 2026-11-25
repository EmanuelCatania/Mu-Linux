#!/bin/bash
set -euo pipefail

echo "MU Linux server 0.97.46 | Cliente compatible: https://github.com/EmanuelCatania/Mu-Linux/releases/tag/0.97.46"

DB_HOST=${DB_HOST:-mysql}
DB_PORT=${DB_PORT:-3306}
DB_USER=${DB_USER:-admin}
DB_PASS=${DB_PASS:-asd123}
DB_NAME=${DB_NAME:-MuOnline97}
PUBLIC_IP=${PUBLIC_IP:-}
MU_ROOT=${MU_ROOT:-/home/container}
children=()

apply_db_settings() {
  local file="$1"
  [ -f "$file" ] || return 0
  sed -i -E "s#^DataBaseHost=.*#DataBaseHost=tcp://${DB_HOST}#" "$file"
  sed -i -E "s#^DataBasePort=.*#DataBasePort=${DB_PORT}#" "$file"
  sed -i -E "s#^DataBaseUser=.*#DataBaseUser=${DB_USER}#" "$file"
  sed -i -E "s#^DataBasePass=.*#DataBasePass=${DB_PASS}#" "$file"
  sed -i -E "s#^DataBaseName=.*#DataBaseName=${DB_NAME}#" "$file"
}

shutdown() {
  local status=${1:-0}
  trap - INT TERM
  echo "Stopping MU services..."
  ((${#children[@]})) && kill -TERM "${children[@]}" 2>/dev/null || true
  wait "${children[@]}" 2>/dev/null || true
  exit "$status"
}

trap 'shutdown 0' INT TERM

apply_db_settings "$MU_ROOT/JoinServer/JoinServer.ini"
apply_db_settings "$MU_ROOT/DataServer/DataServer.ini"

# Cliente de escritorio sólo por el launcher (docs/parches-del-launcher.md).
if [ -f "$MU_ROOT/ConnectServer/ConnectServer.ini" ]; then
  sed -i -E "s/^RequireLauncher *=.*/RequireLauncher=${MU_LAUNCHER_REQUIRED:-0}/" "$MU_ROOT/ConnectServer/ConnectServer.ini"
fi

# MU-Guard (ver server/docker/entrypoint.sh y docs/mu-guard.md).
guard_dir="$MU_ROOT/Data/Guard"
guard_toml="$guard_dir/Guard.toml"
# Builds/: los manifiestos de los main.exe aceptados (mu-guard build).
mkdir -p "$guard_dir/Builds"
if [ ! -f "$guard_toml" ]; then
  cat > "$guard_toml" <<'EOF'
# MU-Guard (docs/mu-guard.md).  Enabled y Required los pisa start.sh con
# MU_GUARD_ENABLED y MU_GUARD_REQUIRED; el resto se edita acá.
Enabled = false
Required = false
Port = 28980
# Socket local GameServer <-> mu-guard; vacío = Data/Guard/mu-guard.sock
Socket = ""
# true: un editor escaneando la memoria del juego (MEMSCAN) lo cierra.
# false: sólo se registra (por si un antivirus da falsos positivos).
MemScanCloses = true
# true: un cliente que no está en Data/Guard/Builds entra igual, sin desafío
# de memoria.  false: sólo los builds de Data/Guard/Builds.
AcceptUnknownBuilds = true
# Cada cuántos segundos se desafía la memoria del juego (5..300).
ChallengeSeconds = 30
# true: una herramienta de Lists.toml abierta cierra el juego.
ToolCloses = true
# Juegos abiertos a la vez en una misma PC (0 = sin límite).
MaxPerMachine = 0
# true: un archivo del juego distinto de Files.txt (mu-guard files) lo cierra.
FilesCloses = true

# Avisos (los cuenta mu-guard en la base): una detección que cierra el juego
# suma uno.  A los `limit` avisos dentro de `window_days` se banea.
[warnings]
limit = 3              # 0 = nunca banear
window_days = 30
# MEMSCAN no suma por defecto: un antivirus que lee la memoria del juego
# lo dispara igual que un editor, y no tiene que terminar en ban.
codes = ["DEBUGGER", "INJECT", "MEMEDIT", "PATCH"]

[ban]
account_days = 7       # 0 = permanente, -1 = no banear la cuenta
machine_days = 7
ip_hours = 24          # nunca IP privadas ni loopback
exempt_accounts = []   # staff: se registran pero no se banean
exempt_ips = []
EOF
fi
# Listas y textos (paso 5c): si no están, se crean con lo de fábrica.
if [ ! -f "$guard_dir/Lists.toml" ]; then
  cat > "$guard_dir/Lists.toml" <<'EOF'
# MU-Guard: listas del server (docs/mu-guard.md).  mu-guard se las manda al
# guardián al conectarse y otra vez cuando cambia este archivo.

# Herramientas (TOOL): si una está abierta mientras se juega, el juego se
# cierra (Guard.toml ToolCloses = false para sólo registrarlas).  No suman
# aviso salvo que se agregue "TOOL" a [warnings] codes.
[tool]
# Nombre del ejecutable, sin importar mayúsculas.
processes = [
  "cheatengine.exe", "cheatengine-x86_64.exe", "cheatengine-x86_64-sse4-avx2.exe", "cheatengine-i386.exe",
  "artmoney.exe", "artmoney64.exe",
  "x64dbg.exe", "x32dbg.exe", "ollydbg.exe",
  "wpe pro.exe", "wpepro.exe",
  "scanmem", "gameconqueror", "pince",
]
# Parte del título de una ventana.  Ojo: "cheat engine" también aparece en el
# título del navegador con esa página abierta.
windows = []

[modules]
# Parte del nombre de un módulo cargado en el juego: lo cierra (INJECT).
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
EOF
fi
if [ ! -f "$guard_dir/Messages.toml" ]; then
  cat > "$guard_dir/Messages.toml" <<'EOF'
# MU-Guard: cómo se nombra cada detección en el banner (docs/mu-guard.md).
# Sin una entrada se usa el texto del cliente.  Detecciones: DEBUGGER, INJECT,
# MEMEDIT, MEMSCAN, PATCH, TOOL.  El banner dice "Se detectó <nombre>".
[es]
# TOOL = "un programa no permitido"

[en]
# TOOL = "A forbidden program"

[pt]
# TOOL = "Um programa proibido"
EOF
fi
# Avisos y bans (paso 5): un Guard.toml de antes no tiene las secciones.
if ! grep -q '^\[warnings\]' "$guard_toml"; then
  cat >> "$guard_toml" <<'EOF'

# Avisos (los cuenta mu-guard en la base): una detección que cierra el juego
# suma uno.  A los `limit` avisos dentro de `window_days` se banea.
[warnings]
limit = 3              # 0 = nunca banear
window_days = 30
# MEMSCAN no suma por defecto: un antivirus que lee la memoria del juego
# lo dispara igual que un editor, y no tiene que terminar en ban.
codes = ["DEBUGGER", "INJECT", "MEMEDIT", "PATCH"]

[ban]
account_days = 7       # 0 = permanente, -1 = no banear la cuenta
machine_days = 7
ip_hours = 24          # nunca IP privadas ni loopback
exempt_accounts = []   # staff: se registran pero no se banean
exempt_ips = []
EOF
fi
guard_bool() { [ "${1:-0}" = "1" ] && echo true || echo false; }
sed -i -E "s/^Enabled *=.*/Enabled = $(guard_bool "${MU_GUARD_ENABLED:-0}")/" "$guard_toml"
sed -i -E "s/^Required *=.*/Required = $(guard_bool "${MU_GUARD_REQUIRED:-0}")/" "$guard_toml"
# El puerto de mu-guard es el que asigna el panel (MU_GUARD_PORT).
sed -i -E "s/^Port *=.*/Port = ${MU_GUARD_PORT:-28980}/" "$guard_toml"

if [ -n "$PUBLIC_IP" ] && [ -f "$MU_ROOT/ConnectServer/ServerList.dat" ]; then
  sed -i -E "s/\"([0-9]{1,3}\.){3}[0-9]{1,3}\"/\"${PUBLIC_IP}\"/g" "$MU_ROOT/ConnectServer/ServerList.dat"
fi

echo "Waiting for MySQL at ${DB_HOST}:${DB_PORT}..."
until (echo >"/dev/tcp/${DB_HOST}/${DB_PORT}") >/dev/null 2>&1; do
  sleep 1
done

export MU_DATA_PATH="$MU_ROOT/Data/"

start_service() {
  local directory="$1" binary="$2"
  (
    cd "$MU_ROOT/$directory"
    exec "./$binary"
  ) &
  children+=("$!")
}

start_service ConnectServer ConnectServer
start_service JoinServer JoinServer
start_service DataServer DataServer
start_service GameServer GameServer

# Traza de paquetes (ver server/docker/entrypoint.sh). El grabador no está en
# `children`, así que si se cae el server sigue.
if [ "${MU_TRACE_ENABLED:-0}" = "1" ] && [ "${MU_TRACE_RECORD:-0}" = "1" ]; then
  (
    for _ in $(seq 1 60); do
      ls /dev/shm/mu-trace-* >/dev/null 2>&1 && break
      sleep 1
    done
    exec "$MU_ROOT/mu-trace" record --out "${MU_TRACE_DIR:-$MU_ROOT/GameServer/LOG/trace}" --keep-mb "${MU_TRACE_KEEP_MB:-1024}"
  ) &
fi

# MU-Guard: fuera de `children` (si se cae, el server sigue y el GameServer
# deja entrar sin validar hasta que vuelva).  Se relanza con espera creciente.
if [ "${MU_GUARD_ENABLED:-0}" = "1" ] && [ -x "$MU_ROOT/mu-guard" ]; then
  (
    cd "$MU_ROOT"
    wait_s=1
    while true; do
      started=$(date +%s)
      "$MU_ROOT/mu-guard" || true
      [ $(( $(date +%s) - started )) -ge 60 ] && wait_s=1
      echo "mu-guard termino; se relanza en ${wait_s}s"
      sleep "$wait_s"
      wait_s=$(( wait_s * 2 )); [ "$wait_s" -gt 30 ] && wait_s=30
    done
  ) &
fi

# Parches del operador (ver server/docker/entrypoint.sh y
# docs/parches-del-launcher.md, "Pterodactyl").  mu-patch publica
# Data/Patches/published; para que la web lo sirva, ese directorio es un Mount
# compartido con el server de la web.  Tampoco entra en `children`.
if [ "${MU_UPDATE_ENABLED:-0}" = "1" ]; then
  patches="$MU_ROOT/Data/Patches"
  key="$MU_ROOT/Data/Keys/ServerIdentity.key"
  mkdir -p "$patches"
  announce=()
  IFS=',' read -r -a list <<< "${MU_LAUNCHER_CONNECT_SERVERS:-${PUBLIC_IP:+${PUBLIC_IP}:44405}}"
  for v in "${list[@]}"; do [ -n "$v" ] && announce+=(--connect-server "$v"); done
  IFS=',' read -r -a list <<< "${MU_UPDATE_URL:-}"
  for v in "${list[@]}"; do [ -n "$v" ] && announce+=(--url "$v"); done
  [ -n "${MU_LAUNCHER_NEWS:-}" ] && announce+=(--news "$MU_LAUNCHER_NEWS")
  [ -n "${MU_LAUNCHER_WEB_URL:-}" ] && announce+=(--web "$MU_LAUNCHER_WEB_URL")
  [ -n "${MU_LAUNCHER_BASE_SOURCE:-}" ] && announce+=(--base-source "$MU_LAUNCHER_BASE_SOURCE")
  [ -n "${MU_LAUNCHER_BASE_TAG:-}" ] && announce+=(--base-tag "$MU_LAUNCHER_BASE_TAG")
  [ -n "${MU_LAUNCHER_BASE_KEY:-}" ] && announce+=(--base-key "$MU_LAUNCHER_BASE_KEY")
  (
    for _ in $(seq 1 120); do
      [ -f "$key" ] && break
      sleep 1
    done
    exec "$MU_ROOT/mu-patch" watch --patches "$patches" --channel "${MU_UPDATE_CHANNEL:-mi-server/overlay}" \
      --key "$key" --interval "${MU_UPDATE_INTERVAL:-30}" "${announce[@]}"
  ) &
fi

echo "GameServer started"
status=0
wait -n "${children[@]}" || status=$?
shutdown "$status"
