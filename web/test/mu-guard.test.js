'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { banStatus, searchFilters, isMissingTable } = require('../lib/mu-guard');

test('un ban levantado se ve levantado aunque no haya vencido', () => {
  const now = new Date('2026-11-25T12:00:00Z');
  assert.equal(banStatus({ lifted: 1, expires_at: new Date('2026-12-01T00:00:00Z') }, now), 'lifted');
});

test('un ban vence con su fecha; sin fecha es permanente', () => {
  const now = new Date('2026-11-25T12:00:00Z');
  assert.equal(banStatus({ lifted: 0, expires_at: new Date('2026-11-25T11:59:00Z') }, now), 'expired');
  assert.equal(banStatus({ lifted: 0, expires_at: new Date('2026-11-25T12:01:00Z') }, now), 'active');
  assert.equal(banStatus({ lifted: 0, expires_at: null }, now), 'active');
});

test('la búsqueda filtra avisos por cuenta, personaje, máquina e IP, y bans por valor', () => {
  const filters = searchFilters('  emapupi  ');
  assert.equal(filters.q, 'emapupi');
  assert.equal(filters.warnings.params.length, 4);
  assert.ok(filters.warnings.params.every((p) => p === '%emapupi%'));
  assert.deepEqual(filters.bans.params, ['%emapupi%']);
});

test('sin búsqueda no hay WHERE', () => {
  const filters = searchFilters('');
  assert.equal(filters.warnings.sql, '');
  assert.equal(filters.bans.sql, '');
});

test('reconoce la tabla que falta', () => {
  assert.ok(isMissingTable({ code: 'ER_NO_SUCH_TABLE' }));
  assert.ok(isMissingTable({ errno: 1146 }));
  assert.ok(!isMissingTable({ code: 'ER_ACCESS_DENIED_ERROR' }));
  assert.ok(!isMissingTable(null));
});
