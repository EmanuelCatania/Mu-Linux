'use strict';

const test = require('node:test');
const assert = require('node:assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const express = require('express');
const { router } = require('../lib/play-files');

function setup() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'jugar-'));
  fs.writeFileSync(path.join(dir, 'main.html'), '<html>juego</html>');
  fs.writeFileSync(path.join(dir, 'main.wasm'), Buffer.from([0, 0x61, 0x73, 0x6d]));
  fs.mkdirSync(path.join(dir, 'game', 'Content'), { recursive: true });
  fs.writeFileSync(path.join(dir, 'game', 'files.txt'), 'Content/a.bmd\t10\n');
  fs.writeFileSync(path.join(dir, 'game', 'Content', 'a.bmd'), '0123456789');
  const app = express();
  app.use('/jugar', router(dir));
  return new Promise((resolve) => {
    const server = app.listen(0, '127.0.0.1', () => resolve({ server, base: `http://127.0.0.1:${server.address().port}` }));
  });
}

test('sin build no publica nada', () => {
  assert.strictEqual(router(''), null);
  assert.strictEqual(router(os.tmpdir() + '/no-existe-jugar'), null);
});

test('sirve el build con aislamiento de origen y CSP propia', async () => {
  const { server, base } = await setup();
  try {
    const page = await fetch(base + '/jugar/');
    assert.strictEqual(page.status, 200);
    assert.strictEqual(await page.text(), '<html>juego</html>');
    assert.strictEqual(page.headers.get('cross-origin-opener-policy'), 'same-origin');
    assert.strictEqual(page.headers.get('cross-origin-embedder-policy'), 'require-corp');
    assert.match(page.headers.get('content-security-policy'), /'wasm-unsafe-eval'/);
    assert.strictEqual(page.headers.get('cache-control'), 'no-cache');

    const wasm = await fetch(base + '/jugar/main.wasm');
    assert.strictEqual(wasm.headers.get('content-type'), 'application/wasm');

    const redirect = await fetch(base + '/jugar', { redirect: 'manual' });
    assert.strictEqual(redirect.status, 301);
  } finally {
    server.close();
  }
});

test('archivos del juego: caché, files.txt sin caché y rangos', async () => {
  const { server, base } = await setup();
  try {
    const file = await fetch(base + '/jugar/game/Content/a.bmd?v=10');
    assert.strictEqual(file.headers.get('cache-control'), 'public, max-age=86400');
    assert.strictEqual(file.headers.get('cross-origin-resource-policy'), 'same-origin');

    const list = await fetch(base + '/jugar/game/files.txt');
    assert.strictEqual(list.headers.get('cache-control'), 'no-cache');

    const part = await fetch(base + '/jugar/game/Content/a.bmd', { headers: { Range: 'bytes=2-4' } });
    assert.strictEqual(part.status, 206);
    assert.strictEqual(await part.text(), '234');

    const escape = await fetch(base + '/jugar/../package.json');
    assert.notStrictEqual(escape.status, 200);
  } finally {
    server.close();
  }
});
