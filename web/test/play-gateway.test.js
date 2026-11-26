'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { attach, acceptKey, decodeFrames, encodeFrame, toServerIp, readConfig, originAllowed } = require('../lib/play-gateway');

test('la clave del saludo es la del ejemplo de la RFC 6455', () => {
  assert.equal(acceptKey('dGhlIHNhbXBsZSBub25jZQ=='), 's3pPLMBiTxaQ9kYGzzhZRbK+xOo=');
});

function masked(opcode, payload, fin = true) {
  const mask = Buffer.from([1, 2, 3, 4]);
  const body = Buffer.from(payload);
  for (let i = 0; i < body.length; i++) body[i] ^= mask[i & 3];
  return Buffer.concat([Buffer.from([(fin ? 0x80 : 0) | opcode, 0x80 | body.length]), mask, body]);
}

test('decodifica marcos enmascarados y guarda el resto', () => {
  const two = Buffer.concat([masked(2, [0xc1, 3, 0xf4]), masked(2, [1, 2])]);
  const partial = two.subarray(0, two.length - 1);
  const { frames, rest } = decodeFrames(partial);
  assert.equal(frames.length, 1);
  assert.deepEqual([...frames[0].payload], [0xc1, 3, 0xf4]);
  assert.equal(rest.length, 7);
});

test('un marco sin máscara no vale', () => {
  assert.equal(decodeFrames(Buffer.from([0x82, 1, 9])), null);
});

test('los marcos del server van sin máscara y con largo extendido', () => {
  assert.deepEqual([...encodeFrame(2, Buffer.from([7]))], [0x82, 1, 7]);
  const big = encodeFrame(2, Buffer.alloc(300));
  assert.equal(big[1], 126);
  assert.equal(big.readUInt16BE(2), 300);
});

test('IP del server: IPv4 tal cual, mapeada a IPv4, IPv6 a 240/4 por su /64', () => {
  assert.equal(toServerIp('1.2.3.4'), '1.2.3.4');
  assert.equal(toServerIp('::ffff:5.6.7.8'), '5.6.7.8');
  const a = toServerIp('2001:db8:1:2::1');
  const b = toServerIp('2001:db8:1:2:ffff::9');
  assert.equal(a, b);
  assert.ok(Number(a.split('.')[0]) >= 240);
  assert.notEqual(a, toServerIp('2001:db8:1:3::1'));
});

test('sin clave larga el puente no se activa; puertos por lista', () => {
  assert.equal(readConfig({ MU_WEB_GATEWAY_KEY: 'corta' }).enabled, false);
  const c = readConfig({ MU_WEB_GATEWAY_KEY: '0123456789abcdef', MU_WEB_PORTS: '44405, 55901' });
  assert.equal(c.enabled, true);
  assert.ok(c.ports.has(55901));
  assert.equal(c.offset, 100);
});

test('origen: el propio y los configurados', () => {
  const config = readConfig({ MU_WEB_ORIGINS: 'http://127.0.0.1:8091' });
  const req = (origin) => ({ headers: { origin, host: 'mu.example' } });
  assert.equal(originAllowed(req('https://mu.example'), config), true);
  assert.equal(originAllowed(req('http://127.0.0.1:8091'), config), true);
  assert.equal(originAllowed(req('https://otro.example'), config), false);
});

test('un upgrade ajeno o con URL inválida se cierra y no tira el proceso', async () => {
  const http = require('http');
  const net = require('net');
  const server = http.createServer((req, res) => res.end('ok'));
  attach(server, { env: { MU_WEB_GATEWAY_KEY: 'k'.repeat(20) }, log: { log() {}, warn() {} } });
  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const closes = (path) => new Promise((resolve, reject) => {
    const socket = net.connect(server.address().port, '127.0.0.1', () => {
      socket.write(`GET ${path} HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n`);
    });
    const timer = setTimeout(() => { socket.destroy(); reject(new Error('sigue abierto: ' + path)); }, 2000);
    socket.on('close', () => { clearTimeout(timer); resolve(); });
    socket.on('error', () => {});
  });
  try {
    await closes('//');
    await closes('/otra');
  } finally {
    server.close();
  }
});
