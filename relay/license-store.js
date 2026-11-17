// RemoteDesk Pro license store, shared by license-server.js (the API) and tools/license.js.
//
// Keys:   RDPRO-XXXXX-XXXXX-XXXXX-XXXXX-XXXXX  (25 Crockford base32 characters = 125 random bits)
// Stored: only SHA-256(normalised key) -> { plan, features, seats, expires, note, revoked, devices }
//         in <LICENSE_DIR>/licenses.json; the key itself is shown once, when it is created.
// Signed: activations return a license text signed with <LICENSE_DIR>/license-signing-key.pem
//         (ECDSA P-256). The app verifies it with the public key compiled into it.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const DIR = path.resolve(process.env.LICENSE_DIR || path.join(__dirname, 'licenses'));
const DB = path.join(DIR, 'licenses.json');
const KEY = path.join(DIR, 'license-signing-key.pem');
const ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ';   // Crockford base32: no I, L, O, U

const FEATURES = { fullscreen: 1, screenshot: 2, recording: 4, files: 8 };
const PLANS = { pro: 1 | 2 | 4 | 8 };

function generateKey() {
  const bytes = crypto.randomBytes(16);   // 128 bits; 125 are used
  let bits = 0n;
  for (const b of bytes) bits = (bits << 8n) | BigInt(b);
  let s = '';
  for (let i = 0; i < 25; i++) { s += ALPHABET[Number(bits & 31n)]; bits >>= 5n; }
  return 'RDPRO-' + s.match(/.{5}/g).join('-');
}

// Accepts any case, spaces/dashes, the RDPRO prefix and the usual look-alikes (O->0, I/L->1).
function normalise(key) {
  let s = String(key || '').toUpperCase().replace(/[^0-9A-Z]/g, '');
  if (s.startsWith('RDPRO')) s = s.slice(5);
  s = s.replace(/O/g, '0').replace(/[IL]/g, '1');
  return /^[0-9A-HJKMNP-TV-Z]{25}$/.test(s) ? s : null;
}

const hashKey = (norm) => crypto.createHash('sha256').update('RemoteDesk-key|' + norm).digest('hex');
const serialOf = (norm) => 'RDPRO-' + norm.slice(0, 5) + '-…';   // safe to show: 25 of 125 bits

function load() {
  if (!fs.existsSync(DB)) return { keys: {} };
  return JSON.parse(fs.readFileSync(DB, 'utf8'));
}

function save(db) {   // write-then-rename, so a crash never leaves half a file
  fs.mkdirSync(DIR, { recursive: true });
  const tmp = DB + '.tmp';
  fs.writeFileSync(tmp, JSON.stringify(db, null, 2) + '\n', { mode: 0o600 });
  fs.renameSync(tmp, DB);
}

function signingKey() {
  if (!fs.existsSync(KEY)) throw new Error(`No license signing key at ${KEY}. Run: node tools/license.js init`);
  return crypto.createPrivateKey(fs.readFileSync(KEY));
}

// The exact text the app verifies; every line is covered by the signature.
function licenseText(entry, serial, device) {
  return [
    'RemoteDesk-license v1',
    `serial=${serial}`,
    `device=${device}`,
    `features=${entry.features}`,
    `expires=${entry.expires || 'never'}`,
    `name=${(entry.note || '').replace(/[\r\n]/g, ' ')}`,
    `issued=${new Date().toISOString().slice(0, 10)}`,
    '',
  ].join('\n');
}

function sign(text) {
  return crypto.sign('sha256', Buffer.from(text), { key: signingKey(), dsaEncoding: 'ieee-p1363' }).toString('hex');
}

module.exports = { DIR, DB, KEY, FEATURES, PLANS, generateKey, normalise, hashKey, serialOf, load, save, licenseText, sign };
