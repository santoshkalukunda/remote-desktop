// RemoteDesk relay / rendezvous server.
//
// Every RemoteDesk client keeps one "control" connection open here and
// registers its 9-digit ID. A viewer asks for an ID, the relay tells that
// host to dial back with a session id, and then both sockets are piped
// together byte-for-byte. Traffic is end-to-end encrypted by the clients,
// so the relay only ever sees ciphertext.
//
//   host  -> REG <id>         <- OK | TAKEN | BAD
//   host  -> PING             <- PONG            (keepalive)
//   relay -> CALL <sid>                          (incoming viewer)
//   host  -> JOIN <sid>       <- READY           (then raw bytes)
//   viewer-> CONN <id>        <- READY | OFFLINE | TIMEOUT | BAD
//
// Run:  node server.js          (PORT env var, default 21116)

'use strict';
const net = require('net');
const crypto = require('crypto');

const PORT = Number(process.env.PORT) || 21116;
const JOIN_TIMEOUT_MS = 60_000;   // host may be showing an accept prompt
const IDLE_TIMEOUT_MS = 75_000;   // control conns ping every 20s

const hosts = new Map();    // id  -> control socket
const pending = new Map();  // sid -> { viewer, timer }

const log = (...a) => console.log(new Date().toISOString(), ...a);

// Read one '\n'-terminated line, then hand back anything after it.
function readLine(sock, cb) {
  let buf = Buffer.alloc(0);
  const onData = (d) => {
    buf = Buffer.concat([buf, d]);
    const i = buf.indexOf(10);
    if (i < 0) { if (buf.length > 256) sock.destroy(); return; }
    sock.removeListener('data', onData);
    sock.pause();
    cb(buf.subarray(0, i).toString('utf8').trim(), buf.subarray(i + 1));
  };
  sock.on('data', onData);
}

function pipe(a, b, restA, restB) {
  a.setTimeout(0); b.setTimeout(0);
  a.write('READY\n'); b.write('READY\n');
  if (restA.length) b.write(restA);
  if (restB.length) a.write(restB);
  a.pipe(b); b.pipe(a);
  const end = () => { a.destroy(); b.destroy(); };
  a.on('close', end); b.on('close', end);
  if (a.destroyed || b.destroyed) { end(); return; }
  a.resume(); b.resume();
}

function handleControl(sock, id, rest) {
  if (!/^\d{9}$/.test(id)) { sock.end('BAD\n'); return; }
  const existing = hosts.get(id);
  if (existing && !existing.destroyed) { sock.end('TAKEN\n'); return; }
  hosts.set(id, sock);
  sock.write('OK\n');
  log('online', id, sock.remoteAddress);

  let buf = rest;
  const onLines = () => {
    let i;
    while ((i = buf.indexOf(10)) >= 0) {
      const line = buf.subarray(0, i).toString().trim();
      buf = buf.subarray(i + 1);
      if (line === 'PING') sock.write('PONG\n');
    }
    if (buf.length > 1024) sock.destroy();
  };
  sock.on('data', (d) => { buf = Buffer.concat([buf, d]); onLines(); });
  sock.on('close', () => {
    if (hosts.get(id) === sock) hosts.delete(id);
    log('offline', id);
  });
  sock.resume();
  onLines();
}

function handleViewer(sock, id, rest) {
  if (!/^\d{9}$/.test(id)) { sock.end('BAD\n'); return; }
  const host = hosts.get(id);
  if (!host || host.destroyed) { sock.end('OFFLINE\n'); return; }
  const sid = crypto.randomBytes(16).toString('hex');
  const timer = setTimeout(() => {
    pending.delete(sid);
    sock.end('TIMEOUT\n');
  }, JOIN_TIMEOUT_MS);
  pending.set(sid, { viewer: sock, rest, timer });
  sock.on('close', () => {
    const p = pending.get(sid);
    if (p && p.viewer === sock) { clearTimeout(p.timer); pending.delete(sid); }
  });
  host.write(`CALL ${sid}\n`);
  log('call', id, 'from', sock.remoteAddress);
}

function handleJoin(sock, sid, rest) {
  const p = pending.get(sid);
  if (!p) { sock.destroy(); return; }
  pending.delete(sid);
  clearTimeout(p.timer);
  pipe(p.viewer, sock, p.rest, rest);
}

const server = net.createServer((sock) => {
  sock.setNoDelay(true);
  sock.setKeepAlive(true, 30_000);
  sock.setTimeout(IDLE_TIMEOUT_MS, () => sock.destroy());
  sock.on('error', () => {});
  readLine(sock, (line, rest) => {
    const [cmd, arg = ''] = line.split(/\s+/, 2);
    if (cmd === 'REG') handleControl(sock, arg, rest);
    else if (cmd === 'CONN') handleViewer(sock, arg, rest);
    else if (cmd === 'JOIN') handleJoin(sock, arg, rest);
    else sock.destroy();
  });
});

server.listen(PORT, () => log(`RemoteDesk relay listening on :${PORT}`));
