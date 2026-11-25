#!/usr/bin/env node
// Server local para probar la versión web: sirve el build (main.html/js/wasm)
// en / y la carpeta del cliente (Content/, System/, files.txt) en /game/, con
// las cabeceras que piden los hilos del wasm (COOP/COEP) y rangos (Range), que
// usan los archivos perezosos para bajar por partes.
//
// Uso: node tools/web/serve.js <carpeta del build> <carpeta del cliente> [puerto]
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');

const [buildDir, clientDir, portArg] = process.argv.slice(2);
if (!buildDir || !clientDir) {
  console.error('Uso: node serve.js <build> <cliente> [puerto]');
  process.exit(2);
}
const port = Number(portArg) || 8080;
const types = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript', '.wasm': 'application/wasm',
                '.txt': 'text/plain; charset=utf-8' };

function resolve(urlPath) {
  const clean = decodeURIComponent(urlPath.split('?')[0]);
  const [base, rel] = clean.startsWith('/game/') ? [clientDir, clean.slice(6)]
                    : [buildDir, clean === '/' ? 'main.html' : clean.slice(1)];
  const full = path.resolve(base, rel);
  return full.startsWith(path.resolve(base)) ? full : null;
}

http.createServer((req, res) => {
  res.setHeader('Cross-Origin-Opener-Policy', 'same-origin');
  res.setHeader('Cross-Origin-Embedder-Policy', 'require-corp');
  res.setHeader('Accept-Ranges', 'bytes');
  const file = resolve(req.url);
  let stat;
  try { stat = file && fs.statSync(file); } catch { stat = null; }
  if (!stat || !stat.isFile()) { res.statusCode = 404; return res.end(); }
  res.setHeader('Content-Type', types[path.extname(file).toLowerCase()] || 'application/octet-stream');
  const range = /bytes=(\d+)-(\d*)/.exec(req.headers.range || '');
  if (range) {
    const start = Number(range[1]);
    const end = range[2] ? Math.min(Number(range[2]), stat.size - 1) : stat.size - 1;
    res.statusCode = 206;
    res.setHeader('Content-Range', `bytes ${start}-${end}/${stat.size}`);
    res.setHeader('Content-Length', end - start + 1);
    if (req.method === 'HEAD') return res.end();
    return fs.createReadStream(file, { start, end }).pipe(res);
  }
  res.setHeader('Content-Length', stat.size);
  if (req.method === 'HEAD') return res.end();
  fs.createReadStream(file).pipe(res);
}).listen(port, '127.0.0.1', () => console.log(`http://127.0.0.1:${port}/`));
