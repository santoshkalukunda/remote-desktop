#!/usr/bin/env node
// RemoteDesk Pro license keys. Run it where the server's license data lives
// (relay/licenses/ next to server.js, or LICENSE_DIR=...).
//
//   node tools/license.js init
//       Once: creates the license signing key (relay/licenses/license-signing-key.pem, keep it private
//       and backed up) and writes its public half to src/core/license_key.h. Rebuild the app afterwards.
//
//   node tools/license.js create [--seats 1] [--days 365 | --expires 2027-12-31] [--note "Customer"] [--count 1]
//       Prints new keys. They are shown only now: the server stores just their SHA-256 hash.
//
//   node tools/license.js list                    keys (by serial), seats used, expiry, status
//   node tools/license.js revoke <key|serial>     stop a key working (apps drop it on their next check)
//   node tools/license.js remove-device <key|serial> <device-prefix>   free one seat
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');
const store = require('../relay/license-store');

const root = path.join(__dirname, '..');
const header = path.join(root, 'src', 'core', 'license_key.h');
const arg = (name, def) => {
  const i = process.argv.indexOf(name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
};

function writeHeader(publicKey) {
  const jwk = publicKey.export({ format: 'jwk' });
  const bytes = Buffer.concat([Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]);
  const rows = [];
  for (let i = 0; i < 64; i += 16)
    rows.push('    ' + [...bytes.subarray(i, i + 16)].map((b) => `0x${b.toString(16).padStart(2, '0')}`).join(', ') + ',');
  fs.writeFileSync(header,
    '#pragma once\n' +
    '// Public half of the RemoteDesk Pro license signing key (ECDSA P-256, X || Y), written by\n' +
    '// `node tools/license.js init`. Licenses must be signed by the matching private key on the server.\n' +
    '#include <cstdint>\n\n' +
    'static const uint8_t LICENSE_PUBLIC_KEY[64] = {\n' + rows.join('\n') + '\n};\n');
}

function init() {
  if (fs.existsSync(store.KEY) && !process.argv.includes('--force')) {
    writeHeader(crypto.createPublicKey(fs.readFileSync(store.KEY)));
    console.log(`${store.KEY} already exists; refreshed ${path.relative(root, header)}.`);
    return;
  }
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ec', { namedCurve: 'P-256' });
  fs.mkdirSync(store.DIR, { recursive: true });
  fs.writeFileSync(store.KEY, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 });
  writeHeader(publicKey);
  console.log(`Created ${store.KEY}  (private: only the server needs it; back it up)`);
  console.log(`Wrote ${path.relative(root, header)}. Rebuild the app so it trusts this key.`);
}

function create() {
  const seats = Math.max(1, parseInt(arg('--seats', '1'), 10) || 1);
  const count = Math.max(1, parseInt(arg('--count', '1'), 10) || 1);
  let expires = arg('--expires', '');
  const days = parseInt(arg('--days', '0'), 10);
  if (!expires && days > 0) expires = new Date(Date.now() + days * 86400_000).toISOString().slice(0, 10);
  if (expires && !/^\d{4}-\d{2}-\d{2}$/.test(expires)) throw new Error('--expires must look like 2027-12-31');
  const note = arg('--note', '');
  const db = store.load();
  const out = [];
  for (let i = 0; i < count; i++) {
    const key = store.generateKey();
    const norm = store.normalise(key);
    db.keys[store.hashKey(norm)] = { plan: 'pro', features: store.PLANS.pro, seats, expires, note,
                                     serial: store.serialOf(norm), created: new Date().toISOString(),
                                     revoked: false, devices: {} };
    out.push(key);
  }
  store.save(db);
  console.log(`Created ${count} key${count > 1 ? 's' : ''}: ${seats} seat${seats > 1 ? 's' : ''}, ` +
              `${expires ? 'expires ' + expires : 'never expires'}${note ? ', note "' + note + '"' : ''}.`);
  console.log('Give these to the customer now; they cannot be shown again:\n');
  for (const k of out) console.log('  ' + k);
}

function find(db, ref) {
  const norm = store.normalise(ref);
  if (norm) return [store.hashKey(norm), db.keys[store.hashKey(norm)]];
  const want = String(ref).toUpperCase().replace(/[^0-9A-Z]/g, '');
  return Object.entries(db.keys).find(([, e]) => e.serial.toUpperCase().replace(/[^0-9A-Z]/g, '') === want) || [null, null];
}

function list() {
  const db = store.load();
  const rows = Object.values(db.keys);
  if (!rows.length) return console.log('No keys yet. Create one: node tools/license.js create --note "Customer"');
  for (const e of rows) {
    const used = Object.keys(e.devices || {}).length;
    const status = e.revoked ? 'REVOKED' : e.expires && e.expires < new Date().toISOString().slice(0, 10) ? 'expired' : 'active';
    console.log(`${e.serial.padEnd(14)} ${status.padEnd(8)} seats ${used}/${e.seats}  ${e.expires || 'never'}  ${e.note || ''}`);
    for (const [d, info] of Object.entries(e.devices || {}))
      console.log(`    ${d.slice(0, 8)}  ${info.computer || ''}  app ${info.app || '?'}  last seen ${(info.lastSeen || '').slice(0, 10)}`);
  }
}

function revoke() {
  const db = store.load();
  const [h, e] = find(db, process.argv[3]);
  if (!e) throw new Error('No such key or serial.');
  e.revoked = true;
  store.save(db);
  console.log(`Revoked ${e.serial}. Apps drop it the next time they connect to the server.`);
}

function removeDevice() {
  const db = store.load();
  const [, e] = find(db, process.argv[3]);
  const prefix = (process.argv[4] || '').toLowerCase();
  if (!e || !prefix) throw new Error('usage: remove-device <key|serial> <device-prefix>');
  const d = Object.keys(e.devices || {}).find((x) => x.startsWith(prefix));
  if (!d) throw new Error('No such device on that key.');
  delete e.devices[d];
  store.save(db);
  console.log(`Freed a seat on ${e.serial}.`);
}

try {
  const cmd = process.argv[2];
  if (cmd === 'init') init();
  else if (cmd === 'create') create();
  else if (cmd === 'list') list();
  else if (cmd === 'revoke') revoke();
  else if (cmd === 'remove-device') removeDevice();
  else console.log('usage: node tools/license.js init | create [--seats N] [--days N|--expires YYYY-MM-DD] [--note ".."] [--count N] | list | revoke <key|serial> | remove-device <key|serial> <device>');
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
