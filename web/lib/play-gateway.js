'use strict';

// Puente WebSocket <-> TCP para la versión web del cliente.
//
// El navegador no abre TCP: el cliente web conecta a /play/ws?port=<puerto> y
// este puente abre una conexión TCP contra el server, al puerto web interno
// (puerto + MU_WEB_PORT_OFFSET), y antes que nada le manda
//
//     MUWEB <MU_WEB_GATEWAY_KEY> <ip del jugador>\n
//
// (ver server/Source/MuServer/Common/WebGateway.h).  Después copia bytes en los
// dos sentidos: lo que viaja es el protocolo del juego tal cual, cifrado por el
// canal del cliente; el puente no lo entiende ni lo toca.
//
// Configuración (variables de entorno):
//   MU_WEB_GATEWAY_KEY   clave compartida con el server (16+ caracteres).  Sin
//                        ella el puente no se activa.
//   MU_WEB_SERVER_HOST   host del server en la red interna (por defecto mu-server).
//   MU_WEB_PORTS         puertos públicos que se pueden pedir, separados por
//                        coma (por defecto 44405,55901).  Cualquier otro se rechaza:
//                        el puente no es un proxy abierto.
//   MU_WEB_PORT_OFFSET   distancia al puerto web interno (por defecto 100).
//   MU_WEB_ORIGINS       orígenes aceptados además del propio, separados por coma.
//
// La IP del jugador sale de la última entrada de X-Forwarded-For (la que
// agrega nuestro proxy): las anteriores las escribe el que conecta.
//
// Sin dependencias: el WebSocket (RFC 6455) está implementado acá, sólo lo que
// hace falta (marcos binarios, ping/pong y cierre).

const crypto = require('crypto');
const net = require('net');

const GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11';
const MAX_FRAME = 1 << 20;
// Topes: conexiones a la vez por IP, minutos sin tráfico y bytes sin entregar
// antes de frenar al que manda.
const MAX_PER_IP = 16;
const IDLE_MS = 10 * 60 * 1000;

function acceptKey(key) {
  return crypto.createHash('sha1').update(key + GUID).digest('base64');
}

function readConfig(env = process.env) {
  const key = env.MU_WEB_GATEWAY_KEY || '';
  return {
    enabled: key.length >= 16,
    key,
    host: env.MU_WEB_SERVER_HOST || 'mu-server',
    ports: new Set((env.MU_WEB_PORTS || '44405,55901').split(',').map((p) => Number(p.trim())).filter(Boolean)),
    offset: env.MU_WEB_PORT_OFFSET ? Number(env.MU_WEB_PORT_OFFSET) : 100,
    origins: new Set((env.MU_WEB_ORIGINS || '').split(',').map((o) => o.trim()).filter(Boolean)),
  };
}

// El server guarda IPv4 (16 bytes).  "::ffff:a.b.c.d" se lleva a IPv4; una IPv6
// de verdad se representa con una dirección fija de 240.0.0.0/4 (reservada, nunca
// es la de alguien) sacada de su /64, así los límites por IP y los bans agrupan
// por red del jugador.
function toServerIp(address) {
  if (!address) return '0.0.0.0';
  const mapped = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/i.exec(address);
  if (mapped) return mapped[1];
  if (net.isIPv4(address)) return address;
  if (!net.isIPv6(address)) return '0.0.0.0';
  const groups = expandIPv6(address).slice(0, 4).join(':');
  const h = crypto.createHash('sha256').update(groups).digest();
  return `${240 | (h[0] & 15)}.${h[1]}.${h[2]}.${h[3]}`;
}

function expandIPv6(address) {
  const [head, tail] = address.split('::');
  const a = head ? head.split(':') : [];
  const b = tail !== undefined && tail ? tail.split(':') : [];
  const fill = new Array(8 - a.length - b.length).fill('0');
  return (tail !== undefined ? [...a, ...fill, ...b] : a).map((g) => g.toLowerCase().padStart(4, '0'));
}

function clientIp(req, trustProxy) {
  if (trustProxy) {
    const forwarded = (req.headers['x-forwarded-for'] || '').split(',').pop().trim();
    if (forwarded) return toServerIp(forwarded);
  }
  return toServerIp(req.socket.remoteAddress);
}

function originAllowed(req, config) {
  const origin = req.headers.origin;
  if (!origin) return true;
  if (config.origins.has(origin)) return true;
  try {
    return new URL(origin).host === req.headers.host;
  } catch {
    return false;
  }
}

function encodeFrame(opcode, payload) {
  const length = payload.length;
  const header = length < 126 ? Buffer.alloc(2) : length < 65536 ? Buffer.alloc(4) : Buffer.alloc(10);
  header[0] = 0x80 | opcode;
  if (length < 126) header[1] = length;
  else if (length < 65536) { header[1] = 126; header.writeUInt16BE(length, 2); }
  else { header[1] = 127; header.writeBigUInt64BE(BigInt(length), 2); }
  return Buffer.concat([header, payload]);
}

// Decodifica los marcos que vengan completos en `buffer`.  Devuelve los marcos y
// lo que sobra (un marco a medias), o null si el flujo no es válido.
function decodeFrames(buffer) {
  const frames = [];
  let offset = 0;
  while (buffer.length - offset >= 2) {
    const fin = (buffer[offset] & 0x80) !== 0;
    const opcode = buffer[offset] & 0x0f;
    const masked = (buffer[offset + 1] & 0x80) !== 0;
    let length = buffer[offset + 1] & 0x7f;
    let pos = offset + 2;
    if (length === 126) {
      if (buffer.length < pos + 2) break;
      length = buffer.readUInt16BE(pos); pos += 2;
    } else if (length === 127) {
      if (buffer.length < pos + 8) break;
      const big = buffer.readBigUInt64BE(pos); pos += 8;
      if (big > BigInt(MAX_FRAME)) return null;
      length = Number(big);
    }
    if (!masked || length > MAX_FRAME) return null; // el cliente siempre enmascara
    if (buffer.length < pos + 4 + length) break;
    const mask = buffer.subarray(pos, pos + 4); pos += 4;
    const payload = Buffer.from(buffer.subarray(pos, pos + length));
    for (let i = 0; i < payload.length; i++) payload[i] ^= mask[i & 3];
    frames.push({ fin, opcode, payload });
    offset = pos + length;
  }
  return { frames, rest: buffer.subarray(offset) };
}

function attach(server, { trustProxy = false, env = process.env, log = console } = {}) {
  const config = readConfig(env);
  if (!config.enabled) return false;
  const perIp = new Map();
  server.on('upgrade', (req, socket) => {
    // Un socket sin oyente de error tira abajo el proceso si se corta.
    socket.on('error', () => {});
    let url;
    try { url = new URL(req.url, 'http://x'); } catch { socket.destroy(); return; }
    // No hay otro upgrade en el sitio: lo que no es del puente se cierra (si
    // no, el socket queda abierto para siempre).
    if (url.pathname !== '/play/ws') { socket.destroy(); return; }
    const port = Number(url.searchParams.get('port'));
    const key = req.headers['sec-websocket-key'];
    const ip = clientIp(req, trustProxy);
    if (!key || !config.ports.has(port) || !originAllowed(req, config) || (perIp.get(ip) || 0) >= MAX_PER_IP) {
      socket.end('HTTP/1.1 403 Forbidden\r\n\r\n');
      return;
    }
    perIp.set(ip, (perIp.get(ip) || 0) + 1);
    const protocol = (req.headers['sec-websocket-protocol'] || '').split(',')[0].trim();
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
      + `Sec-WebSocket-Accept: ${acceptKey(key)}\r\n`
      + (protocol ? `Sec-WebSocket-Protocol: ${protocol}\r\n` : '') + '\r\n');
    socket.setNoDelay(true);

    const game = net.connect(port + config.offset, config.host);
    game.setNoDelay(true);
    let pending = Buffer.alloc(0);
    let message = [];
    let messageSize = 0;
    let closed = false;
    const close = () => {
      if (closed) return;
      closed = true;
      const left = (perIp.get(ip) || 1) - 1;
      if (left > 0) perIp.set(ip, left); else perIp.delete(ip);
      try { socket.write(encodeFrame(0x8, Buffer.alloc(0))); } catch { /* ya cerrado */ }
      socket.destroy();
      game.destroy();
    };
    socket.setTimeout(IDLE_MS, close);

    game.on('connect', () => game.write(`MUWEB ${config.key} ${ip}\n`));
    // Si un lado no da abasto, el otro espera: sin esto lo pendiente se
    // acumula en memoria sin tope.
    game.on('data', (data) => { if (!socket.write(encodeFrame(0x2, data))) game.pause(); });
    socket.on('drain', () => game.resume());
    game.on('drain', () => socket.resume());
    game.on('error', (e) => { log.warn(`[play] ${ip} -> ${port}: ${e.message}`); close(); });
    game.on('close', close);

    socket.on('data', (data) => {
      pending = pending.length ? Buffer.concat([pending, data]) : data;
      const decoded = decodeFrames(pending);
      if (!decoded) return close();
      pending = decoded.rest;
      for (const frame of decoded.frames) {
        if (frame.opcode === 0x8) return close();
        if (frame.opcode === 0x9) { socket.write(encodeFrame(0xA, frame.payload)); continue; }
        if (frame.opcode === 0xA) continue;
        messageSize += frame.payload.length;
        if (messageSize > MAX_FRAME) return close(); // mensaje partido en marcos sin fin
        message.push(frame.payload);
        if (frame.fin) {
          if (!game.write(Buffer.concat(message))) socket.pause();
          message = []; messageSize = 0;
        }
      }
    });
    socket.on('error', close);
    socket.on('close', close);
  });
  log.log(`[play] puente WebSocket activo -> ${config.host} (puertos ${[...config.ports].join(', ')} + ${config.offset})`);
  return true;
}

module.exports = { attach, acceptKey, decodeFrames, encodeFrame, toServerIp, readConfig, originAllowed };
