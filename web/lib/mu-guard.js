'use strict';

// Panel de MU-Guard (paso 5b): avisos y bans que escribe mu-guard en
// MuGuardWarning y MuGuardBan (docs/mu-guard.md, "Avisos y bans").  mu-guard
// crea las tablas al arrancar; sin ellas el panel lo dice en vez de fallar.

const KINDS = ['account', 'machine', 'ip'];

const LIMIT = 200;

// Estado de un ban: levantado a mano, vencido o vigente.
function banStatus(row, now = new Date()) {
  if (Number(row.lifted)) return 'lifted';
  if (row.expires_at) {
    const expires = row.expires_at instanceof Date ? row.expires_at : new Date(row.expires_at);
    if (!Number.isNaN(expires.getTime()) && expires <= now) return 'expired';
  }
  return 'active';
}

// Búsqueda por cuenta, personaje, máquina o IP (texto parcial).  Devuelve el
// WHERE de cada tabla con sus parámetros.
function searchFilters(raw) {
  const q = String(raw || '').trim().slice(0, 64);
  if (!q) {
    return { q, warnings: { sql: '', params: [] }, bans: { sql: '', params: [] } };
  }
  const like = `%${q}%`;
  return {
    q,
    warnings: {
      sql: 'WHERE account LIKE ? OR character_name LIKE ? OR machine LIKE ? OR ip LIKE ?',
      params: [like, like, like, like]
    },
    bans: { sql: 'WHERE value LIKE ?', params: [like] }
  };
}

// Las tablas no existen todavía: mu-guard nunca arrancó con base.
function isMissingTable(error) {
  return Boolean(error) && (error.code === 'ER_NO_SUCH_TABLE' || error.errno === 1146);
}

module.exports = { KINDS, LIMIT, banStatus, searchFilters, isMissingTable };
