#!/bin/bash
# Arma el launcher de Linux como AppImage: un solo archivo con las bibliotecas
# que no son del sistema (OpenSSL...) adentro, que corre en cualquier distro.
#
#   appimage.sh <launcher compilado> <archivo de salida> [título]
#
# El resultado va en Launcher/launcher de la carpeta del juego, igual que el
# binario suelto: el launcher sabe que está en un AppImage (variable APPIMAGE)
# y toma de ahí la carpeta del juego y lo que se actualiza a sí mismo.
#
# Baja linuxdeploy (versión fija, verificada por hash).  No necesita FUSE.
set -euo pipefail

if [ $# -lt 2 ]; then
  echo "uso: $0 <launcher> <salida.AppImage> [título]" >&2
  exit 2
fi
launcher=$(readlink -f "$1")
out=$(readlink -f "$(dirname "$2")")/$(basename "$2")
title=${3:-Mu Online}
here=$(cd "$(dirname "$0")" && pwd)

LINUXDEPLOY_URL=https://github.com/linuxdeploy/linuxdeploy/releases/download/1-alpha-20251107-1/linuxdeploy-x86_64.AppImage
LINUXDEPLOY_SHA256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

curl -fsSL -o "$work/linuxdeploy" "$LINUXDEPLOY_URL"
echo "$LINUXDEPLOY_SHA256  $work/linuxdeploy" | sha256sum -c --quiet -
chmod +x "$work/linuxdeploy"

app="$work/AppDir"
mkdir -p "$app/usr/bin"
install -m 755 "$launcher" "$app/usr/bin/launcher"
cp "$here/mu-launcher.png" "$work/mu-launcher.png"
cat > "$work/mu-launcher.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$title
Exec=launcher
Icon=mu-launcher
Categories=Game;
Terminal=false
EOF

cd "$work"
APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 OUTPUT="$out" ./linuxdeploy --appdir "$app" \
  --executable "$app/usr/bin/launcher" --desktop-file mu-launcher.desktop --icon-file mu-launcher.png \
  --output appimage
chmod +x "$out"
echo "listo: $out"
