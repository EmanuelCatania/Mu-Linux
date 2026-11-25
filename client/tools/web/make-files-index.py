#!/usr/bin/env python3
"""Arma files.txt para la versión web: una línea por archivo de Content/ y
System/ con su tamaño ("ruta<TAB>bytes"), rutas relativas a la carpeta del
cliente y con barras '/'.  La página crea con esto el árbol de archivos del
wasm; el contenido se baja recién cuando el juego abre cada archivo.

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
    lines = []
    for folder in FOLDERS:
        base = os.path.join(root, folder)
        if not os.path.isdir(base):
            continue
        for current, _dirs, files in os.walk(base):
            for name in files:
                full = os.path.join(current, name)
                rel = os.path.relpath(full, root).replace(os.sep, '/')
                lines.append('%s\t%d' % (rel, os.path.getsize(full)))
    lines.sort(key=str.lower)
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('%d archivos -> %s' % (len(lines), out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
