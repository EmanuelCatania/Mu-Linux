'use strict';

// Archivos de la versión web del cliente, en /jugar.
//
// MU_WEB_CLIENT_DIR tiene el build (main.html, main.js, main.wasm y la variante
// main-asyncify.* para Firefox) y, en game/, los archivos del juego con su
// files.txt (client/tools/web/make-files-index.py).  La página baja cada
// archivo del juego recién cuando lo necesita (lib perezosa de la página).
//
// Cabeceras:
//  - COOP/COEP/CORP: sin aislamiento de origen el navegador no da
//    SharedArrayBuffer, y el wasm usa hilos.
//  - CSP propia (no la de helmet): el wasm se compila ('wasm-unsafe-eval') y
//    los hilos y el audio arrancan desde blob:.  La red va por el WebSocket de
//    /play/ws (lib/play-gateway.js), del mismo origen.
//  - Caché: el build se revalida siempre (cambia con cada versión).  Los
//    archivos del juego llevan ?v=<tamaño> en la URL, así que se pueden guardar
//    un día; files.txt se revalida siempre.

const path = require('path');
const fs = require('fs');
const express = require('express');

const CSP = [
  "default-src 'self'",
  "script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval' blob:",
  "worker-src 'self' blob:",
  "connect-src 'self' ws: wss: blob:",
  "img-src 'self' data: blob:",
  "style-src 'self' 'unsafe-inline'",
  "font-src 'self' data:",
  "base-uri 'self'",
  "frame-ancestors 'self'"
].join('; ');

function isolation(req, res, next) {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Cross-Origin-Resource-Policy', 'same-origin');
  res.setHeader('X-Content-Type-Options', 'nosniff');
  res.setHeader('Content-Security-Policy', CSP);
  next();
}

function cacheHeaders(res, file) {
  const rel = file.split(path.sep).join('/');
  if (/\/game\//.test(rel) && !/\/game\/files\.txt$/i.test(rel)) {
    res.setHeader('Cache-Control', 'public, max-age=86400');
  } else {
    res.setHeader('Cache-Control', 'no-cache');
  }
}

// null si no hay build en la carpeta: /jugar no se publica.
function router(dir) {
  if (!dir || !fs.existsSync(path.join(dir, 'main.html'))) return null;
  const r = express.Router();
  r.use(isolation);
  r.use(express.static(dir, { index: 'main.html', dotfiles: 'ignore', setHeaders: cacheHeaders }));
  return r;
}

module.exports = { router, CSP };
