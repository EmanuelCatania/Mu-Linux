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
