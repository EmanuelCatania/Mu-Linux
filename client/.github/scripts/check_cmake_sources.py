#!/usr/bin/env python3
"""Verifica las listas de fuentes del CMakeLists.txt del cliente.

MU_SOURCES y MU_HEADERS son listas explícitas (sin glob): este script falla si
alguna entrada está repetida o apunta a un archivo que no existe.  Antes las
comparaba contra mu97k.vcxproj; el proyecto de Visual Studio se quitó junto con
los builds x86 y CMake quedó como única fuente.

Sale con código 1 si hay cualquier problema. Lo corre el CI.

Uso: python .github/scripts/check_cmake_sources.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def cmake_list(path, name):
    with open(path, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"set\(%s\s*\n(.*?)\)" % re.escape(name), text, re.S)
    if not m:
        raise SystemExit("%s: no se encontró set(%s ...)" % (path, name))
    items = []
    for line in m.group(1).splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            items.append(line)
    return items


def check(label, items):
    ok = True
    dups = sorted({x for x in items if items.count(x) > 1})
    if dups:
        print("[%s] duplicados: %s" % (label, dups))
        ok = False
    for x in items:
        if not os.path.isfile(os.path.join(ROOT, x)):
            print("[%s] el archivo no existe: %s" % (label, x))
            ok = False
    print("[%s] %d entradas: %s" % (label, len(items), "OK" if ok else "ERROR"))
    return ok


def main():
    cmake = os.path.join(ROOT, "CMakeLists.txt")
    results = [
        check("MU_SOURCES", cmake_list(cmake, "MU_SOURCES")),
        check("MU_HEADERS", cmake_list(cmake, "MU_HEADERS")),
    ]
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
