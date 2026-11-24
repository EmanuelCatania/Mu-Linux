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
mkdir -p "$guard_dir"
if [ ! -f "$guard_toml" ]; then
  cat > "$guard_toml" <<'EOF'
# MU-Guard (docs/mu-guard.md).  Enabled y Required los pisa el entrypoint con
# MU_GUARD_ENABLED y MU_GUARD_REQUIRED; el resto se edita acá.
Enabled = false
Required = false
Port = 55980
# Socket local GameServer <-> mu-guard; vacío = Data/Guard/mu-guard.sock
Socket = ""
EOF
fi
guard_bool() { [ "${1:-0}" = "1" ] && echo true || echo false; }
sed -i -E "s/^Enabled *=.*/Enabled = $(guard_bool "${MU_GUARD_ENABLED:-0}")/" "$guard_toml"
sed -i -E "s/^Required *=.*/Required = $(guard_bool "${MU_GUARD_REQUIRED:-0}")/" "$guard_toml"

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
