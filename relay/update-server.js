// RemoteDesk update server: a tiny static HTTP server for in-app updates.
//
//   GET /latest.json      manifest written by `node tools/release.js publish`
//   GET /files/<name>     the signed installer
//   POST /api/...         RemoteDesk Pro license activation (license-server.js)
//
// Started by server.js on UPDATE_PORT (default 21117; set UPDATE_PORT=0 to disable),
// or on its own:  node update-server.js
// Serve it behind HTTPS (e.g. a reverse proxy) when you can; the app also verifies
// every installer's SHA-256 and the release signature before running it.
'use strict';
const http = require('http');
const fs = require('fs');
const path = require('path');
const license = require('./license-server');

const DIR = path.resolve(process.env.UPDATE_DIR || path.join(__dirname, 'updates'));
const log = (...a) => console.log(new Date().toISOString(), '[update]', ...a);

function send(res, code, body, type = 'text/plain') {
  res.writeHead(code, { 'Content-Type': type, 'Cache-Control': 'no-store' });
  res.end(body);
}

function handler(req, res) {
  if ((req.url || '').startsWith('/api/')) return license.handle(req, res);   // RemoteDesk Pro activation
  if (req.method !== 'GET' && req.method !== 'HEAD') return send(res, 405, 'method not allowed');
  const url = decodeURIComponent((req.url || '/').split('?')[0]);
  if (url === '/latest.json') {
    const f = path.join(DIR, 'latest.json');
    if (!fs.existsSync(f)) return send(res, 404, 'no release published yet');
    return send(res, 200, fs.readFileSync(f), 'application/json');
  }
  const m = url.match(/^\/files\/([A-Za-z0-9._-]+)$/);   // plain file names only: no traversal
  if (!m) return send(res, 404, 'not found');
  const f = path.join(DIR, 'files', m[1]);
  fs.stat(f, (err, st) => {
    if (err || !st.isFile()) return send(res, 404, 'not found');
    res.writeHead(200, { 'Content-Type': 'application/octet-stream', 'Content-Length': st.size });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(f).pipe(res);
    log('download', m[1], 'by', req.socket.remoteAddress);
  });
}

function start(port) {
  const server = http.createServer(handler);
  server.on('error', (e) => log('error:', e.message));
  server.listen(port, () => log(`serving ${DIR} on :${port}`));
  return server;
}

module.exports = { start };
if (require.main === module) start(Number(process.env.UPDATE_PORT) || 21117);
