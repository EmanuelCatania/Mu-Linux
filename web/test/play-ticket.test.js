'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const crypto = require('crypto');
const playTicket = require('../lib/play-ticket');

test('el pase entra en el campo de contraseña del juego', () => {
  for (let i = 0; i < 200; i++) assert.match(playTicket.make(), /^[0-9A-Za-z]{10}$/);
});

test('dos pases seguidos no coinciden', () => {
  assert.notEqual(playTicket.make(), playTicket.make());
});

test('el hash es el SHA-256 en hexadecimal (el que calcula el JoinServer con SHA2)', () => {
  assert.equal(playTicket.hash('abc'), crypto.createHash('sha256').update('abc').digest('hex'));
  assert.equal(playTicket.hash('abc').length, 64);
});

test('emitir guarda el hash, no el pase, y usa el nombre de cuenta de la base', async () => {
  const queries = [];
  const db = {
    async query(sql, params) {
      queries.push([sql, params]);
      return sql.startsWith('SELECT') ? [[{ memb___id: 'Tester' }]] : [{}];
    }
  };
  const result = await playTicket.issue(db, 'tester');
  assert.equal(result.account, 'Tester');
  const [sql, params] = queries[1];
  assert.match(sql, /^REPLACE INTO web_play_ticket/);
  assert.deepEqual(params, ['Tester', playTicket.hash(result.ticket), playTicket.LIFETIME_SECONDS]);
  assert.ok(!JSON.stringify(params).includes(result.ticket));
});

test('una cuenta que no existe no recibe pase', async () => {
  const db = { async query() { return [[]]; } };
  assert.equal(await playTicket.issue(db, 'nadie'), null);
});
