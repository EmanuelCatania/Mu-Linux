'use strict';

// Pase de un solo uso para entrar al juego desde la web (/jugar) sin escribir
// la contraseña.
//
// Con sesión iniciada en el sitio, la página del juego pide un pase (POST
// /play/ticket) y lo manda en el campo de contraseña del login.  El JoinServer,
// si la contraseña no coincide, busca el pase en web_play_ticket: si está
// vigente lo borra y deja entrar (JoinServerProtocol.cpp, WebTicketConsume).
//
// - 10 caracteres [0-9A-Za-z]: lo que entra en el campo de contraseña del
//   protocolo.  Son 62^10 combinaciones para un pase que vive un minuto.
// - Se guarda sólo su SHA-256: quien lea la tabla no puede usarlo.
// - Uno por cuenta: pedir otro invalida el anterior.

const crypto = require('crypto');

const ALPHABET = '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz';
const LENGTH = 10;
const LIFETIME_SECONDS = 60;

const TABLE_SQL = `
  CREATE TABLE IF NOT EXISTS web_play_ticket (
    account_id VARCHAR(10) NOT NULL,
    token_hash CHAR(64) NOT NULL,
    expires_at DATETIME NOT NULL,
    PRIMARY KEY (account_id)
  )
`;

function make() {
  let ticket = '';
  for (let i = 0; i < LENGTH; i++) ticket += ALPHABET[crypto.randomInt(ALPHABET.length)];
  return ticket;
}

function hash(ticket) {
  return crypto.createHash('sha256').update(ticket, 'latin1').digest('hex');
}

// Devuelve { account, ticket } con el nombre de la cuenta como está en la base,
// o null si la cuenta no existe.
async function issue(db, accountId) {
  const [rows] = await db.query('SELECT memb___id FROM MEMB_INFO WHERE memb___id = ? LIMIT 1', [accountId]);
  if (rows.length === 0) return null;
  const account = rows[0].memb___id;
  const ticket = make();
  await db.query(
    'REPLACE INTO web_play_ticket (account_id, token_hash, expires_at) VALUES (?, ?, DATE_ADD(NOW(), INTERVAL ? SECOND))',
    [account, hash(ticket), LIFETIME_SECONDS]
  );
  return { account, ticket };
}

module.exports = { ALPHABET, LENGTH, LIFETIME_SECONDS, TABLE_SQL, make, hash, issue };
