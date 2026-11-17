#!/usr/bin/env node
// Release tooling for in-app updates.
//
//   node tools/release.js keygen
//       Once per project: creates the release signing key (keys/update-signing-key.pem,
//       never commit or lose it) and writes its public half into src/core/update_key.h.
//       Rebuild the app afterwards so it trusts the key.
//
//   node tools/release.js publish [build/setup.exe] [--notes "What's new"] [--out relay/updates]
//       (--version X overrides the number from version.h; meant for testing only)
//       Signs build/setup.exe as the version in src/core/version.h and places it where
//       relay/update-server.js serves it:  <out>/latest.json  and  <out>/files/<setup>.
//       Copy that folder to the server (or run this on it) to roll the update out.
//
// The app installs an update only if the download's SHA-256 matches latest.json and the
// manifest's ECDSA P-256 signature verifies against the compiled-in public key.
'use strict';
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const root = path.join(__dirname, '..');
const keyFile = path.join(root, 'keys', 'update-signing-key.pem');
const headerFile = path.join(root, 'src', 'core', 'update_key.h');

function arg(name, def) {
  const i = process.argv.indexOf(name);
  return i > 0 && process.argv[i + 1] ? process.argv[i + 1] : def;
}

function appVersion() {
  const h = fs.readFileSync(path.join(root, 'src', 'core', 'version.h'), 'utf8');
  const n = (k) => h.match(new RegExp(`#define\\s+RD_VERSION_${k}\\s+(\\d+)`))[1];
  return `${n('MAJOR')}.${n('MINOR')}.${n('PATCH')}`;
}

function writeHeader(publicKey) {
  const jwk = publicKey.export({ format: 'jwk' });
  const bytes = Buffer.concat([Buffer.from(jwk.x, 'base64url'), Buffer.from(jwk.y, 'base64url')]);
  const rows = [];
  for (let i = 0; i < 64; i += 16) rows.push('    ' + [...bytes.subarray(i, i + 16)].map((b) => `0x${b.toString(16).padStart(2, '0')}`).join(', ') + ',');
  fs.writeFileSync(headerFile,
    '#pragma once\n' +
    '// Public half of the release signing key (ECDSA P-256, X || Y), written by\n' +
    '// `node tools/release.js keygen`. Updates must be signed with the matching private key.\n' +
    '#include <cstdint>\n\n' +
    'static const uint8_t UPDATE_PUBLIC_KEY[64] = {\n' + rows.join('\n') + '\n};\n');
}

function keygen() {
  if (fs.existsSync(keyFile) && !process.argv.includes('--force')) {
    console.log(`${path.relative(root, keyFile)} already exists; refreshing the public key header only.`);
    writeHeader(crypto.createPublicKey(fs.readFileSync(keyFile)));
    return;
  }
  const { privateKey, publicKey } = crypto.generateKeyPairSync('ec', { namedCurve: 'P-256' });
  fs.mkdirSync(path.dirname(keyFile), { recursive: true });
  fs.writeFileSync(keyFile, privateKey.export({ format: 'pem', type: 'pkcs8' }), { mode: 0o600 });
  writeHeader(publicKey);
  console.log(`Created ${path.relative(root, keyFile)} (keep it safe and private: it is the only key that can publish updates)`);
  console.log(`Wrote ${path.relative(root, headerFile)}. Rebuild the app so it trusts this key.`);
}

// node tools/release.js bump 1.2.1  -> rewrites the numbers in src/core/version.h
function bump() {
  const v = process.argv[3] || '';
  const m = v.match(/^(\d+)\.(\d+)\.(\d+)$/);
  if (!m) throw new Error('usage: node tools/release.js bump <major.minor.patch>, e.g. 1.2.1');
  const file = path.join(root, 'src', 'core', 'version.h');
  let h = fs.readFileSync(file, 'utf8');
  for (const [k, n] of [['MAJOR', m[1]], ['MINOR', m[2]], ['PATCH', m[3]]])
    h = h.replace(new RegExp(`(#define\\s+RD_VERSION_${k}\\s+)\\d+`), `$1${n}`);
  fs.writeFileSync(file, h);
  console.log(`Version set to ${v} in src/core/version.h`);
}

function publish() {
  const setup = path.resolve(process.argv[3] && !process.argv[3].startsWith('--') ? process.argv[3] : path.join(root, 'build', 'setup.exe'));
  const out = path.resolve(arg('--out', path.join(root, 'relay', 'updates')));
  const notes = arg('--notes', '');
  if (!fs.existsSync(keyFile)) throw new Error('No signing key. Run: node tools/release.js keygen');
  if (!fs.existsSync(setup)) throw new Error(`${setup} not found. Build the installer first.`);

  const version = arg('--version', appVersion());   // override only for testing
  const data = fs.readFileSync(setup);
  const sha256 = crypto.createHash('sha256').update(data).digest('hex');
  const message = `RemoteDesk-update\n${version}\n${sha256}\n`;
  const signature = crypto.sign('sha256', Buffer.from(message), {
    key: fs.readFileSync(keyFile), dsaEncoding: 'ieee-p1363',
  }).toString('hex');

  const file = `RemoteDesk-setup-${version}.exe`;
  fs.mkdirSync(path.join(out, 'files'), { recursive: true });
  fs.copyFileSync(setup, path.join(out, 'files', file));
  const manifest = { version, file: `files/${file}`, size: data.length, sha256, notes, signature,
                     published: new Date().toISOString() };
  fs.writeFileSync(path.join(out, 'latest.json'), JSON.stringify(manifest, null, 2) + '\n');
  console.log(`Published ${version} -> ${path.relative(root, out)}/latest.json (${(data.length / 1048576).toFixed(1)} MB)`);
}

const cmd = process.argv[2];
try {
  if (cmd === 'keygen') keygen();
  else if (cmd === 'bump') bump();
  else if (cmd === 'publish') publish();
  else console.log('usage: node tools/release.js keygen | bump <x.y.z> | publish [setup.exe] [--notes "..."] [--out dir]');
} catch (e) {
  console.error(e.message);
  process.exit(1);
}
