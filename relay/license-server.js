// RemoteDesk Pro license API, served under /api/ by update-server.js (port 21117).
//
//   POST /api/activate    { key, device, computer, app }  -> { ok, license (base64), signature (hex) }
//   POST /api/deactivate  { serial, device }              -> { ok }   frees this PC's seat
//   POST /api/check       { serial, device }              -> { valid } revoked / removed seats
//
// Keys are looked up by their SHA-256 hash only. Failed activations are rate-limited per IP.
'use strict';
const store = require('./license-store');

const log = (...a) => console.log(new Date().toISOString(), '[license]', ...a);
const WINDOW_MS = 10 * 60_000, MAX_FAILS = 10;
const fails = new Map();   // ip -> { count, since }

function tooMany(ip) {
  const f = fails.get(ip);
  if (!f || Date.now() - f.since > WINDOW_MS) return false;
  return f.count >= MAX_FAILS;
}
function failed(ip) {
  const f = fails.get(ip);
  if (!f || Date.now() - f.since > WINDOW_MS) fails.set(ip, { count: 1, since: Date.now() });
  else f.count++;
}

function reply(res, code, obj) {
  res.writeHead(code, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store' });
  res.end(JSON.stringify(obj));
}

function readBody(req, cb) {
  let data = '';
  req.on('data', (d) => { data += d; if (data.length > 4096) req.destroy(); });
  req.on('end', () => { try { cb(JSON.parse(data || '{}')); } catch { cb(null); } });
}

const validDevice = (d) => typeof d === 'string' && /^[0-9a-f]{32}$/.test(d);
const today = () => new Date().toISOString().slice(0, 10);

function activate(ip, body, res) {
  if (tooMany(ip)) return reply(res, 429, { ok: false, error: 'Too many attempts. Try again in a few minutes.' });
  const norm = store.normalise(body.key);
  if (!norm || !validDevice(body.device)) { failed(ip); return reply(res, 400, { ok: false, error: 'That is not a valid RemoteDesk Pro key.' }); }
  const db = store.load();
  const hash = store.hashKey(norm);
  const entry = db.keys[hash];
  if (!entry) { failed(ip); log('unknown key from', ip); return reply(res, 404, { ok: false, error: 'This key was not found. Check it and try again.' }); }
  if (entry.revoked) return reply(res, 403, { ok: false, error: 'This key has been revoked.' });
  if (entry.expires && entry.expires < today()) return reply(res, 403, { ok: false, error: `This key expired on ${entry.expires}.` });
  entry.devices = entry.devices || {};
  if (!entry.devices[body.device] && Object.keys(entry.devices).length >= entry.seats)
    return reply(res, 403, { ok: false, error: `This key is already used on ${entry.seats} computer${entry.seats === 1 ? '' : 's'}. Deactivate one first.` });
  const serial = store.serialOf(norm);
  entry.devices[body.device] = { computer: String(body.computer || '').slice(0, 64), app: String(body.app || '').slice(0, 16),
                                 activated: entry.devices[body.device]?.activated || new Date().toISOString(),
                                 lastSeen: new Date().toISOString() };
  entry.serial = serial;
  store.save(db);
  const text = store.licenseText(entry, serial, body.device);
  log('activated', serial, 'on', body.device.slice(0, 8), entry.devices[body.device].computer);
  reply(res, 200, { ok: true, license: Buffer.from(text).toString('base64'), signature: store.sign(text) });
}

function findBySerial(db, serial) {
  return Object.values(db.keys).find((e) => e.serial && e.serial === serial);
}

function deactivate(body, res) {
  const db = store.load();
  const entry = findBySerial(db, body.serial);
  if (entry && entry.devices && validDevice(body.device) && entry.devices[body.device]) {
    delete entry.devices[body.device];
    store.save(db);
    log('deactivated', body.serial, 'on', body.device.slice(0, 8));
  }
  reply(res, 200, { ok: true });
}

function check(body, res) {
  const db = store.load();
  const entry = findBySerial(db, body.serial);
  if (!entry || !validDevice(body.device)) return reply(res, 200, { valid: false, error: 'This license is not known to the server.' });
  if (entry.revoked) return reply(res, 200, { valid: false, error: 'This license has been revoked.' });
  if (!entry.devices || !entry.devices[body.device]) return reply(res, 200, { valid: false, error: 'This computer was removed from the license.' });
  entry.devices[body.device].lastSeen = new Date().toISOString();
  store.save(db);
  reply(res, 200, { valid: true });
}

function handle(req, res) {
  if (req.method !== 'POST') return reply(res, 405, { ok: false, error: 'POST only' });
  const ip = req.socket.remoteAddress;
  readBody(req, (body) => {
    if (!body) return reply(res, 400, { ok: false, error: 'Bad request' });
    try {
      if (req.url === '/api/activate') return activate(ip, body, res);
      if (req.url === '/api/deactivate') return deactivate(body, res);
      if (req.url === '/api/check') return check(body, res);
      reply(res, 404, { ok: false, error: 'Not found' });
    } catch (e) {
      log('error:', e.message);
      reply(res, 500, { ok: false, error: 'The license server is not set up yet.' });
    }
  });
}

module.exports = { handle };
