'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const express = require('express');
const { updatesRouter } = require('../lib/updates');

async function withServer(dir, options, fn) {
  const app = express();
  app.use('/updates', updatesRouter(dir, options));
  const server = await new Promise((resolve) => {
    const s = app.listen(0, '127.0.0.1', () => resolve(s));
  });
  try {
    await fn(`http://127.0.0.1:${server.address().port}/updates`);
  } finally {
    await new Promise((resolve) => server.close(resolve));
  }
}

function publishedDir() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'mu-updates-'));
  fs.writeFileSync(path.join(dir, 'manifest.json'), '{"format":1}');
  fs.writeFileSync(path.join(dir, 'manifest.json.sig'), 'abcd\n');
  fs.writeFileSync(path.join(dir, 'launcher.json'), '{"format":1,"sequence":1}');
  fs.writeFileSync(path.join(dir, 'launcher.json.sig'), 'ef01\n');
  fs.writeFileSync(path.join(dir, 'srv-overlay-1.0.1-1.zip'), Buffer.from('0123456789abcdef'));
  fs.mkdirSync(path.join(dir, 'history', '1'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'history', '1', 'manifest.json'), '{}');
  fs.writeFileSync(path.join(dir, 'notas.txt'), 'privado');
  return dir;
}

test('sirve el manifiesto, launcher.json, las firmas y los zips; nada más', async () => {
  const dir = publishedDir();
  await withServer(dir, {}, async (base) => {
    let r = await fetch(`${base}/manifest.json`);
    assert.equal(r.status, 200);
    assert.equal(await r.text(), '{"format":1}');
    assert.equal(r.headers.get('cache-control'), 'no-cache');
    r = await fetch(`${base}/manifest.json.sig`);
    assert.equal(r.status, 200);
    r = await fetch(`${base}/launcher.json`);
    assert.equal(r.status, 200);
    assert.equal(r.headers.get('cache-control'), 'no-cache');
    r = await fetch(`${base}/launcher.json.sig`);
    assert.equal(r.status, 200);
    r = await fetch(`${base}/srv-overlay-1.0.1-1.zip`);
    assert.equal(r.status, 200);
    assert.match(r.headers.get('cache-control'), /immutable/);
    for (const p of ['/history/1/manifest.json', '/notas.txt', '/', '/../x.zip', '/falta.zip', '/%2e%2e/manifest.json']) {
      r = await fetch(`${base}${p}`);
      assert.equal(r.status, 404, p);
    }
    r = await fetch(`${base}/manifest.json`, { method: 'POST' });
    assert.equal(r.status, 405);
  });
});

test('un rango por pedido: 206 con el tramo pedido', async () => {
  const dir = publishedDir();
  await withServer(dir, {}, async (base) => {
    const r = await fetch(`${base}/srv-overlay-1.0.1-1.zip`, { headers: { Range: 'bytes=4-9' } });
    assert.equal(r.status, 206);
    assert.equal(r.headers.get('content-range'), 'bytes 4-9/16');
    assert.equal(await r.text(), '456789');
  });
});

test('límite propio por IP', async () => {
  const dir = publishedDir();
  await withServer(dir, { perMinute: 3 }, async (base) => {
    for (let i = 0; i < 3; ++i) assert.equal((await fetch(`${base}/manifest.json`)).status, 200);
    assert.equal((await fetch(`${base}/manifest.json`)).status, 429);
  });
});
