'use strict';

// Parches del operador para el launcher (docs/parches-del-launcher.md).
//
// mu-server los publica en Data/Patches/published (mu-patch watch) y mu-web los
// sirve en /updates/ desde un volumen montado de sólo lectura.  Sólo se sirven
// el manifiesto, su firma y los zips de esa carpeta: nunca una carpeta de
// parche del admin a medio subir, ni history/, ni nada más.
//
// - Range: express.static responde un rango por pedido (206).  A un pedido con
//   varios rangos contesta el archivo entero; el launcher pide de a uno.
// - Caché: el manifiesto cambia con cada parche (no-cache, con ETag); un zip no
//   cambia nunca, porque su nombre lleva la versión (inmutable).
// - Límite propio por IP: el general del sitio no cubre los estáticos.

const express = require('express');
const rateLimit = require('express-rate-limit');

const ALLOWED = /^(manifest\.json|manifest\.json\.sig|[A-Za-z0-9][A-Za-z0-9._-]*\.zip)$/;

function updatesRouter(dir, { perMinute = 120 } = {}) {
  const router = express.Router();
  router.use(rateLimit({
    windowMs: 60 * 1000,
    max: Math.max(1, perMinute),
    standardHeaders: true,
    legacyHeaders: false
  }));
  router.use((req, res, next) => {
    if (req.method !== 'GET' && req.method !== 'HEAD') return res.status(405).end();
    const name = req.path.replace(/^\//, '');
    if (!ALLOWED.test(name)) return res.status(404).end();
    next();
  });
  router.use(express.static(dir, {
    index: false,
    dotfiles: 'deny',
    fallthrough: false,
    etag: true,
    lastModified: true,
    setHeaders(res, file) {
      if (file.endsWith('.zip')) res.setHeader('Cache-Control', 'public, max-age=31536000, immutable');
      else res.setHeader('Cache-Control', 'no-cache');
    }
  }));
  // fallthrough: false -> un archivo que falta es un 404 sin página de error.
  router.use((err, req, res, next) => {
    if (res.headersSent) return next(err);
    res.status(err.status || err.statusCode || 500).end();
  });
  return router;
}

module.exports = { updatesRouter, ALLOWED };
