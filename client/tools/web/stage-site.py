#!/usr/bin/env python3
"""Arma la carpeta que sirve mu-web en /jugar (MU_WEB_CLIENT_PATH):

    <salida>/main.html, main.js, main.wasm, main-asyncify.*   el build
    <salida>/game/Content, game/System                         los del juego
    <salida>/game/files.txt                                    su índice

Copia sólo lo que cambió (tamaño o fecha), así que sirve también para
actualizar una carpeta ya armada.  Lo que no se redistribuye queda afuera
(los audios de los emotes, ver .gitignore): el juego sigue sin ese sonido.

Uso: stage-site.py <carpeta del build> <carpeta del cliente> <salida>
"""
import hashlib
import importlib.util
import os
import shutil
import sys

BUILD_FILES = ('main.html', 'main.js', 'main.wasm', 'main-asyncify.js', 'main-asyncify.wasm')
FOLDERS = ('Content', 'System')
# Rutas (relativas al cliente, en minúsculas) que no se publican.
EXCLUDED = ('content/audio/sound/dances/',)


def copy_if_changed(src, dst):
    try:
        s, d = os.stat(src), os.stat(dst)
        if s.st_size == d.st_size and int(s.st_mtime) == int(d.st_mtime):
            return False
    except FileNotFoundError:
        pass
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def main():
    if len(sys.argv) != 4:
        print(__doc__)
        return 2
    build, client, out = (os.path.abspath(p) for p in sys.argv[1:])
    copied = 0
    digest = hashlib.sha1()
    for name in BUILD_FILES:
        src = os.path.join(build, name)
        if not os.path.isfile(src):
            print('Falta %s en el build' % src)
            return 1
        if name == 'main.html':
            continue
        with open(src, 'rb') as f:
            digest.update(f.read())
        copied += copy_if_changed(src, os.path.join(out, name))
    # La página pide main.js y main.wasm con ?v=<hash del build> (ver BUILD en
    # shell.html): un proxy que guarda los .js no mezcla versiones.
    with open(os.path.join(build, 'main.html'), encoding='utf-8') as f:
        page = f.read().replace('%MU_BUILD%', digest.hexdigest()[:12])
    page_out = os.path.join(out, 'main.html')
    old = open(page_out, encoding='utf-8').read() if os.path.isfile(page_out) else None
    if page != old:
        os.makedirs(out, exist_ok=True)
        with open(page_out, 'w', encoding='utf-8', newline='') as f:
            f.write(page)
        copied += 1

    game = os.path.join(out, 'game')
    wanted = set()
    for folder in FOLDERS:
        for current, _dirs, files in os.walk(os.path.join(client, folder)):
            for name in files:
                full = os.path.join(current, name)
                rel = os.path.relpath(full, client).replace(os.sep, '/')
                if rel.lower().startswith(EXCLUDED):
                    continue
                wanted.add(rel)
                copied += copy_if_changed(full, os.path.join(game, rel))
    # Lo que ya no está en el cliente (o quedó excluido) sale de la carpeta.
    removed = 0
    for folder in FOLDERS:
        for current, _dirs, files in os.walk(os.path.join(game, folder)):
            for name in files:
                full = os.path.join(current, name)
                if os.path.relpath(full, game).replace(os.sep, '/') not in wanted:
                    os.remove(full)
                    removed += 1

    spec = importlib.util.spec_from_file_location(
        'make_files_index', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'make-files-index.py'))
    index = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(index)
    sys.argv = [sys.argv[0], game]
    index.main()
    print('%d copiados, %d borrados -> %s' % (copied, removed, out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
