#!/usr/bin/env python3
"""Arma files.txt para la versión web: una línea por archivo de Content/ y
System/ con su tamaño ("ruta<TAB>bytes"), rutas relativas a la carpeta del
cliente y con barras '/'.  La página crea con esto el árbol de archivos del
wasm; el contenido se baja recién cuando el juego abre cada archivo.

Los que lista boot-files.txt (junto a este script: lo que el cliente abre hasta
la pantalla de login) llevan una tercera columna "b": la página los baja de
fondo mientras el jugador está en el menú previo.  Para rehacer esa lista,
llegar al login en el navegador y copiar Module.muRequested (una ruta por
línea).

Uso: make-files-index.py <carpeta del cliente> [<salida>]
     (por defecto la salida es <carpeta>/files.txt)
"""
import os
import sys

FOLDERS = ('Content', 'System')


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    root = os.path.abspath(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, 'files.txt')
    boot = set()
    boot_list = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'boot-files.txt')
    if os.path.isfile(boot_list):
        with open(boot_list, encoding='utf-8') as f:
            boot = {line.strip().lower() for line in f if line.strip() and not line.startswith('#')}
    lines = []
    for folder in FOLDERS:
        base = os.path.join(root, folder)
        if not os.path.isdir(base):
            continue
        for current, _dirs, files in os.walk(base):
            for name in files:
                full = os.path.join(current, name)
                rel = os.path.relpath(full, root).replace(os.sep, '/')
                line = '%s\t%d' % (rel, os.path.getsize(full))
                if rel.lower() in boot:
                    line += '\tb'
                lines.append(line)
    lines.sort(key=str.lower)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d archivos -> %s' % (len(lines), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
