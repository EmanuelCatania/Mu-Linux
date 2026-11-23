#!/usr/bin/env python3
"""Arma los canales del cliente (base, música y HD) para adjuntarlos a una Release.

Cada canal sale de client/bin/Client con mu-patch y queda en una carpeta plana
lista para `gh release upload`:

    mu-linux-base.manifest.json   (+ .sig)
    mu-linux-base-<versión>-1.zip ...

Los assets de una Release comparten un solo espacio de nombres, por eso el
manifiesto lleva el canal en el nombre. Los zips ya lo llevan (mu-patch los
nombra <canal>-<versión>-N.zip). Un zip con `url` vacía está en la misma
Release que el manifiesto.

Con --fetch-prev busca en las Releases ya publicadas las 3 versiones más nuevas
anteriores a la que se arma: reusa sus zips para lo que no cambió (quedan
apuntando a su Release) y baja sólo los que hacen falta para los deltas.
La secuencia sale de la más alta publicada en todo el canal + 1, así una 0.97.47
publicada después de la 0.97.50 igual queda por encima y el launcher no la
rechaza.

Sin dependencias fuera de la biblioteca estándar.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.error
import urllib.request

CHANNELS = [
    {
        "channel": "mu-linux/base",
        "include": ["Content/**", "System/**"],
        "exclude": ["Content/**/*-hd.*", "Content/audio/music/**"],
        "optional": [],
    },
    {
        "channel": "mu-linux/music",
        "include": ["Content/audio/music/**"],
        "exclude": [],
        "optional": ["**"],
    },
    {
        "channel": "mu-linux/hd",
        "include": ["Content/**/*-hd.*"],
        "exclude": [],
        "optional": ["**"],
    },
]

DELTA_MIN_SIZE = 1 << 20   # kDeltaMinSize de PatchBuilder.cpp
DELTA_HISTORY = 3          # kDeltaHistory
VERSION_RE = re.compile(r"^[0-9]+(\.[0-9]+)+$")


def slug(channel):
    return channel.replace("/", "-")


def version_key(v):
    return tuple(int(p) for p in v.split("."))


def log(msg):
    print(msg, flush=True)


def http_get(url, token=None, accept=None):
    req = urllib.request.Request(url)
    req.add_header("User-Agent", "mu-linux-client-channels")
    if token and url.startswith("https://api.github.com/"):
        req.add_header("Authorization", "Bearer " + token)
    if accept:
        req.add_header("Accept", accept)
    with urllib.request.urlopen(req, timeout=120) as r:
        return r.read()


def download(url, path, token=None):
    tmp = path + ".part"
    req = urllib.request.Request(url, headers={"User-Agent": "mu-linux-client-channels"})
    with urllib.request.urlopen(req, timeout=600) as r, open(tmp, "wb") as f:
        shutil.copyfileobj(r, f, 1 << 20)
    os.replace(tmp, path)


def changed(input_dir, entry):
    """True si el archivo del manifiesto anterior existe en la entrada con otro contenido."""
    path = os.path.join(input_dir, *entry["path"].split("/"))
    if not os.path.isfile(path):
        return False
    if os.path.getsize(path) != entry["size"]:
        return True
    h = hashlib.blake2b(digest_size=32)
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return "blake2b-256:" + h.hexdigest() != entry["hash"]


def list_releases(repo, token):
    out, page = [], 1
    while True:
        data = json.loads(http_get(
            "https://api.github.com/repos/%s/releases?per_page=100&page=%d" % (repo, page),
            token, "application/vnd.github+json"))
        if not data:
            return out
        out.extend(data)
        page += 1


def run(cmd):
    log("+ " + " ".join(cmd))
    subprocess.run(cmd, check=True)


def fetch_previous(repo, token, tag, workdir, input_dir):
    """Por canal: (secuencia más alta publicada, [(carpeta, url_de_la_release)])."""
    releases = [r for r in list_releases(repo, token)
                if not r.get("draft") and VERSION_RE.match(r["tag_name"]) and r["tag_name"] != tag]
    result = {}
    for ch in CHANNELS:
        name = slug(ch["channel"])
        published = []   # (versión, secuencia, manifiesto, base_url, release)
        for r in releases:
            assets = {a["name"]: a for a in r.get("assets", [])}
            if name + ".manifest.json" not in assets or name + ".manifest.json.sig" not in assets:
                continue
            base_url = "https://github.com/%s/releases/download/%s" % (repo, r["tag_name"])
            try:
                text = http_get(base_url + "/" + name + ".manifest.json")
                manifest = json.loads(text)
            except (urllib.error.URLError, ValueError) as e:
                log("aviso: no se pudo leer el manifiesto %s de %s: %s" % (name, r["tag_name"], e))
                continue
            published.append((r["tag_name"], int(manifest.get("sequence", 0)), text, base_url))

        max_seq = max((p[1] for p in published), default=0)
        older = sorted((p for p in published if version_key(p[0]) < version_key(tag)),
                       key=lambda p: version_key(p[0]), reverse=True)[:DELTA_HISTORY]
        prevs = []
        for version, _, text, base_url in older:
            d = os.path.join(workdir, name, version)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "manifest.json"), "wb") as f:
                f.write(text)
            download(base_url + "/" + name + ".manifest.json.sig", os.path.join(d, "manifest.json.sig"))
            manifest = json.loads(text)
            # Sólo los zips con archivos grandes que cambiaron: son los únicos
            # que pueden llevar delta.  El resto se reusa por URL sin bajarlo.
            needed = {f["bundle"] for f in manifest.get("files", [])
                      if f.get("size", 0) >= DELTA_MIN_SIZE and changed(input_dir, f)}
            for i in sorted(needed):
                b = manifest["bundles"][i]
                url = b.get("url") or (base_url + "/" + b["name"])
                log("bajando %s (%s)" % (b["name"], version))
                download(url, os.path.join(d, b["name"]))
            prevs.append((d, base_url))
        log("%s: secuencia publicada %d, versiones anteriores: %s"
            % (ch["channel"], max_seq, ", ".join(v for v, *_ in older) or "ninguna"))
        result[ch["channel"]] = (max_seq, prevs)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mu-patch", required=True)
    ap.add_argument("--in", dest="input", required=True, help="client/bin/Client")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--key", required=True)
    ap.add_argument("--out", required=True, help="carpeta plana para la Release")
    ap.add_argument("--work", required=True, help="carpeta de trabajo")
    ap.add_argument("--repo", default=os.environ.get("GITHUB_REPOSITORY", ""))
    ap.add_argument("--fetch-prev", action="store_true")
    ap.add_argument("--level", default="19")
    a = ap.parse_args()

    if not VERSION_RE.match(a.tag):
        sys.exit("el tag %r no es una versión (por ejemplo 0.97.50)" % a.tag)
    if os.path.exists(a.out) and os.listdir(a.out):
        sys.exit("la carpeta de salida no está vacía: " + a.out)
    os.makedirs(a.out, exist_ok=True)

    pubkey = subprocess.run([a.mu_patch, "pubkey", "--key", a.key], check=True,
                            capture_output=True, text=True).stdout.strip()
    log("clave pública de la Release: " + pubkey)

    previous = {}
    if a.fetch_prev:
        if not a.repo:
            sys.exit("--fetch-prev necesita --repo o GITHUB_REPOSITORY")
        previous = fetch_previous(a.repo, os.environ.get("GITHUB_TOKEN"), a.tag,
                                  os.path.join(a.work, "prev"), a.input)

    staged = set()
    for ch in CHANNELS:
        name = slug(ch["channel"])
        out = os.path.join(a.work, "out", name)
        max_seq, prevs = previous.get(ch["channel"], (0, []))
        cmd = [a.mu_patch, "build", "--channel", ch["channel"], "--version", a.tag,
               "--in", a.input, "--out", out, "--key", a.key, "--level", a.level]
        for g in ch["include"]:
            cmd += ["--include", g]
        for g in ch["exclude"]:
            cmd += ["--exclude", g]
        for g in ch["optional"]:
            cmd += ["--optional", g]
        for d, url in prevs:
            cmd += ["--prev", d + "@" + url]
        if max_seq:
            cmd += ["--sequence", str(max_seq + 1)]
        run(cmd)

        verify = [a.mu_patch, "verify", "--in", out, "--pubkey", pubkey]
        for d, _ in prevs:
            verify += ["--prev", d]
        run(verify)

        for f in sorted(os.listdir(out)):
            dst = name + "." + f if f.startswith("manifest.json") else f
            if dst in staged:
                sys.exit("nombre repetido entre canales: " + dst)
            staged.add(dst)
            shutil.copyfile(os.path.join(out, f), os.path.join(a.out, dst))

    total = sum(os.path.getsize(os.path.join(a.out, f)) for f in staged)
    log("listo: %d archivos, %.1f MB en %s" % (len(staged), total / 1048576.0, a.out))


if __name__ == "__main__":
    main()
