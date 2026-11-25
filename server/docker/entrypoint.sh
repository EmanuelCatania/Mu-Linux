#!/usr/bin/env bash
set -euo pipefail

echo "MU Linux server 0.97.46 | Cliente compatible: https://github.com/EmanuelCatania/Mu-Linux/releases/tag/0.97.46"

DB_HOST=${DB_HOST:-mysql}
DB_PORT=${DB_PORT:-3306}
DB_USER=${DB_USER:-admin}
DB_PASS=${DB_PASS:-asd123}
DB_NAME=${DB_NAME:-MuOnline97}
PUBLIC_IP=${PUBLIC_IP:-}
MU_DATA_PATH=${MU_DATA_PATH:-/opt/mu/Data/}

apply_db_settings() {
  local file="$1"
  if [ -f "$file" ]; then
    sed -i -E "s#^DataBaseHost=.*#DataBaseHost=tcp://${DB_HOST}#" "$file"
    sed -i -E "s#^DataBasePort=.*#DataBasePort=${DB_PORT}#" "$file"
    sed -i -E "s#^DataBaseUser=.*#DataBaseUser=${DB_USER}#" "$file"
    sed -i -E "s#^DataBasePass=.*#DataBasePass=${DB_PASS}#" "$file"
    sed -i -E "s#^DataBaseName=.*#DataBaseName=${DB_NAME}#" "$file"
  fi
}

apply_db_settings /opt/mu/JoinServer/JoinServer.ini
apply_db_settings /opt/mu/DataServer/DataServer.ini

# Cliente de escritorio sólo por el launcher (docs/parches-del-launcher.md).
if [ -f /opt/mu/ConnectServer/ConnectServer.ini ]; then
  sed -i -E "s/^RequireLauncher *=.*/RequireLauncher=${MU_LAUNCHER_REQUIRED:-0}/" /opt/mu/ConnectServer/ConnectServer.ini
fi

# MU-Guard (docs/mu-guard.md): Data/Guard/Guard.toml lo leen el ConnectServer,
# el GameServer y mu-guard.  Data/ es un volumen: si el archivo no está (una
# instalación anterior) se crea acá, y Enabled/Required salen de las variables.
guard_dir="${MU_DATA_PATH%/}/Guard"
guard_toml="$guard_dir/Guard.toml"
# Builds/: los manifiestos de los main.exe aceptados (mu-guard build).
mkdir -p "$guard_dir/Builds"
if [ ! -f "$guard_toml" ]; then
  cat > "$guard_toml" <<'EOF'
# MU-Guard (docs/mu-guard.md).  Enabled y Required los pisa el entrypoint con
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
# true: se puede entrar con la versión web (navegador), que no puede tener
# guardián: entra sin ticket aunque Required esté en true, sólo con los
# controles del server.  false: la versión web no entra.
WebClients = true

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
# Listas de fábrica: se reescriben en cada arranque, así un server ya instalado
# recibe las nuevas.  Las del operador van en Lists.toml y se suman a estas.
cat > "$guard_dir/Lists.default.toml" <<'EOF'
# MU-Guard: listas de fábrica (docs/mu-guard.md).  NO EDITAR: el arranque las
# reescribe en cada versión.  Para agregar las tuyas, usá Lists.toml: mu-guard
# suma los dos archivos.

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
# Las del operador: si no están, un archivo con las secciones vacías.
if [ ! -f "$guard_dir/Lists.toml" ]; then
  cat > "$guard_dir/Lists.toml" <<'EOF'
# MU-Guard: tus listas (docs/mu-guard.md).  Se suman a las de fábrica
# (Lists.default.toml, que no se edita).  mu-guard se las manda al guardián al
# conectarse y otra vez cuando cambian.
[tool]
# Nombre del ejecutable, sin importar mayúsculas: si está abierto, TOOL.
processes = []
# Parte del título de una ventana.
windows = []

[modules]
# Parte del nombre de un módulo cargado en el juego: lo cierra (INJECT).
block = []
# Parte de la ruta de un módulo que no hace falta registrar.
allow = []
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
# El puerto también: el 55980 de una versión de prueba chocaba con el DataServer.
sed -i -E "s/^Port *=.*/Port = ${MU_GUARD_PORT:-28980}/" "$guard_toml"

if [ -n "$PUBLIC_IP" ] && [ -f /opt/mu/ConnectServer/ServerList.dat ]; then
  sed -i -E "s/\"([0-9]{1,3}\.){3}[0-9]{1,3}\"/\"${PUBLIC_IP}\"/g" /opt/mu/ConnectServer/ServerList.dat
fi

if command -v nc >/dev/null 2>&1; then
  echo "Waiting for MySQL at ${DB_HOST}:${DB_PORT}..."
  until nc -z "$DB_HOST" "$DB_PORT"; do
    sleep 1
  done
fi

export MU_DATA_PATH

cd /opt/mu/ConnectServer
./ConnectServer &

cd /opt/mu/JoinServer
./JoinServer &

cd /opt/mu/DataServer
./DataServer &

cd /opt/mu/GameServer
./GameServer &

# Traza de paquetes: MU_TRACE_ENABLED=1 deja el anillo disponible (se mira con
# `mu-trace tail`); MU_TRACE_RECORD=1 además lo graba a archivos rotativos.
# El grabador no entra en el `wait -n`: si se cae, el server sigue.
if [ "${MU_TRACE_ENABLED:-0}" = "1" ] && [ "${MU_TRACE_RECORD:-0}" = "1" ]; then
  (
    for _ in $(seq 1 60); do
      ls /dev/shm/mu-trace-* >/dev/null 2>&1 && break
      sleep 1
    done
    exec /opt/mu/mu-trace record --out "${MU_TRACE_DIR:-/opt/mu/GameServer/LOG/trace}" --keep-mb "${MU_TRACE_KEEP_MB:-1024}"
  ) &
  disown $!
fi

# MU-Guard: mu-guard no entra en el `wait -n` (si se cae, el server sigue y el
# GameServer deja entrar sin validar hasta que vuelva).  Se relanza con una
# espera que crece de 1 a 30 s.
if [ "${MU_GUARD_ENABLED:-0}" = "1" ] && [ -x /opt/mu/mu-guard ]; then
  (
    wait_s=1
    while true; do
      started=$(date +%s)
      /opt/mu/mu-guard || true
      # Corrió un buen rato: la próxima caída vuelve a esperar poco.
      [ $(( $(date +%s) - started )) -ge 60 ] && wait_s=1
      echo "mu-guard termino; se relanza en ${wait_s}s"
      sleep "$wait_s"
      wait_s=$(( wait_s * 2 )); [ "$wait_s" -gt 30 ] && wait_s=30
    done
  ) &
  disown $!
fi

# Parches del operador (docs/parches-del-launcher.md): con MU_UPDATE_ENABLED=1,
# mu-patch publica Data/Patches/published cada vez que el admin sube una
# carpeta DDMMAAAA-N.  Firma con la identidad del server, que se crea sola
# la primera vez que arranca el server.  Como el grabador de trazas, no entra en el
# `wait -n`: si se cae, el server sigue.
if [ "${MU_UPDATE_ENABLED:-0}" = "1" ]; then
  patches="${MU_DATA_PATH%/}/Patches"
  key="${MU_DATA_PATH%/}/Keys/ServerIdentity.key"
  mkdir -p "$patches"
  # launcher.json (la configuración que reparte el ConnectServer): direcciones
  # del ConnectServer, URL pública de /updates/, noticias y origen de la base.
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
    exec /opt/mu/mu-patch watch --patches "$patches" --channel "${MU_UPDATE_CHANNEL:-mi-server/overlay}" \
      --key "$key" --interval "${MU_UPDATE_INTERVAL:-30}" "${announce[@]}"
  ) &
  disown $!
fi

wait -n
