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
//   host  -> BYE                                 (going to sleep: offline at once)
//   relay -> CALL <sid>                          (incoming viewer)
//   host  -> JOIN <sid>       <- READY           (then raw bytes)
//   viewer-> CONN <id>        <- READY | OFFLINE | TIMEOUT | BAD
//   host  -> NET <macs> <ip/prefix,..>          (network cards, for Wake-on-LAN)
//   viewer-> WAKE <id>        <- SENT <n> | INET | UNKNOWN | ONLINE | BAD
//   relay -> WAKE <macs> <broadcasts> <ips>     (to up to 5 online hosts on the target's network)
//
// Wake-on-LAN: a switched-off PC can only be woken from inside its own network, so the relay
// remembers each host's network cards (wol.json, survives restarts) and, when someone wants to
// reach it while it's offline, asks other RemoteDesk computers on the same network (same public
// address, or the same local subnet) to send the magic packet. The relay also sends it itself, to
// the computer's public address (office -> home works if that router forwards UDP 9 to the PC or
// its network) and to its subnet broadcast (works when the relay is on the same network). INET
// tells the viewer that only this internet route was available.
//
// Run:  node server.js          (PORT env var, default 21116)

'use strict';
const net = require('net');
const crypto = require('crypto');
const fs = require('fs');
const dgram = require('dgram');
const path = require('path');

const PORT = Number(process.env.PORT) || 21116;
const JOIN_TIMEOUT_MS = 60_000;   // host may be showing an accept prompt
const IDLE_TIMEOUT_MS = 75_000;   // control conns ping every 20s

const hosts = new Map();    // id  -> control socket
const WOL_FILE = process.env.WOL_FILE || path.join(__dirname, 'wol.json');
const WOL_KEEP_MS = 90 * 24 * 3600_000;   // forget computers not seen for 90 days
const WAKE_GAP_MS = 20_000;               // per computer: one wake-up request at a time
let wol = {};                             // id -> { macs: [..], nets: ['192.168.1.23/24'], pub, seen }
try { wol = JSON.parse(fs.readFileSync(WOL_FILE, 'utf8')); } catch { wol = {}; }
const lastWake = new Map();
let wolTimer = null;
function saveWol() {   // debounced, write-then-rename
  if (wolTimer) return;
  wolTimer = setTimeout(() => {
    wolTimer = null;
    const now = Date.now();
    for (const [id, e] of Object.entries(wol)) if (now - e.seen > WOL_KEEP_MS) delete wol[id];
    try { fs.mkdirSync(path.dirname(WOL_FILE), { recursive: true }); fs.writeFileSync(WOL_FILE + '.tmp', JSON.stringify(wol)); fs.renameSync(WOL_FILE + '.tmp', WOL_FILE); }
    catch (e) { log('wol save failed', e.message); }
  }, 2000);
}
const cleanAddr = (a) => String(a || '').replace(/^::ffff:/, '');
const ipNum = (ip) => ip.split('.').reduce((n, x) => (n * 256 + (Number(x) & 255)) >>> 0, 0);
function parseNet(cidr) {   // '192.168.1.23/24' -> { net, mask, bcast } or null
  const m = /^(\d{1,3}(?:\.\d{1,3}){3})\/(\d{1,2})$/.exec(cidr);
  if (!m || Number(m[2]) > 32 || Number(m[2]) < 8) return null;
  const mask = Number(m[2]) === 0 ? 0 : (0xFFFFFFFF << (32 - Number(m[2]))) >>> 0;
  const ip = ipNum(m[1]);
  const net = (ip & mask) >>> 0;
  const b = (net | (~mask >>> 0)) >>> 0;
  return { net, mask, bcast: [b >>> 24, (b >>> 16) & 255, (b >>> 8) & 255, b & 255].join('.') };
}
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

// A host pings after 25 s without traffic, so one that has been silent longer than this has
// gone to sleep or lost its network (no FIN arrives then). Treating it as offline right away
// lets a viewer wake it, instead of waiting for IDLE_TIMEOUT_MS and then timing out on CALL.
const HOST_SILENT_MS = 35_000;
function liveHost(id) {
  const h = hosts.get(id);
  if (!h || h.destroyed) return null;
  if (Date.now() - h.lastSeen > HOST_SILENT_MS) { log('silent', id); h.destroy(); hosts.delete(id); return null; }
  return h;
}

function handleControl(sock, id, rest) {
  if (!/^\d{9}$/.test(id)) { sock.end('BAD\n'); return; }
  if (liveHost(id)) { sock.end('TAKEN\n'); return; }
  sock.lastSeen = Date.now();
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
      else if (line === 'BYE') {   // going to sleep: offline now, so viewers wake it instead of calling
        if (hosts.get(id) === sock) hosts.delete(id);
        sock.end();
        return;
      } else if (line.startsWith('NET ')) {
        const [, macs = '', nets = ''] = line.split(' ');
        const m = macs.split(',').filter((x) => /^[0-9a-f]{2}(-[0-9a-f]{2}){5}$/i.test(x)).slice(0, 8);
        const n = nets.split(',').filter((x) => parseNet(x)).slice(0, 8);
        if (m.length) { wol[id] = { macs: m, nets: n, pub: cleanAddr(sock.remoteAddress), seen: Date.now() }; saveWol(); }
        sock.nets = n;
      }
    }
    if (buf.length > 1024) sock.destroy();
  };
  sock.on('data', (d) => { sock.lastSeen = Date.now(); buf = Buffer.concat([buf, d]); onLines(); });
  sock.on('close', () => {
    if (hosts.get(id) === sock) hosts.delete(id);
    log('offline', id);
  });
  sock.resume();
  onLines();
}

function handleViewer(sock, id, rest) {
  if (!/^\d{9}$/.test(id)) { sock.end('BAD\n'); return; }
  const host = liveHost(id);
  if (!host) { sock.end('OFFLINE\n'); return; }
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

// The magic packet (6 x FF + the MAC 16 times) for each MAC, to each address, on UDP 9 and 7.
function sendMagic(macs, addrs) {
  const u = dgram.createSocket('udp4');
  u.on('error', () => {});
  u.bind(() => {
    try { u.setBroadcast(true); } catch {}
    let left = 0;
    for (const m of macs) {
      const mac = Buffer.from(m.replace(/-/g, ''), 'hex');
      const pkt = Buffer.concat([Buffer.alloc(6, 0xff), ...Array(16).fill(mac)]);
      for (const a of addrs) for (const port of [9, 7]) { left++; u.send(pkt, port, a, () => { if (--left === 0) u.close(); }); }
    }
    if (!left) u.close();
  });
}

function handleWake(sock, id) {
  if (!/^\d{9}$/.test(id)) { sock.end('BAD\n'); return; }
  if (liveHost(id)) { sock.end('ONLINE\n'); return; }
  const e = wol[id];
  if (!e) { sock.end('UNKNOWN\n'); return; }
  const now = Date.now();
  if (now - (lastWake.get(id) || 0) < WAKE_GAP_MS) { sock.end('SENT 1\n'); return; }   // already on its way
  const targetNets = e.nets.map(parseNet).filter(Boolean);
  const sameNetwork = (h) => {
    if (cleanAddr(h.remoteAddress) === e.pub) return true;   // behind the same internet connection
    return (h.nets || []).map(parseNet).filter(Boolean)
      .some((a) => targetNets.some((b) => a.mask === b.mask && a.net === b.net));
  };
  const helpers = [];
  for (const [hid] of [...hosts]) {
    const h = hid === id ? null : liveHost(hid);   // a sleeping helper can't send anything
    if (!h || !sameNetwork(h)) continue;
    helpers.push(h);
    if (helpers.length >= 5) break;
  }
  lastWake.set(id, now);
  const bcastList = targetNets.map((n) => n.bcast);
  const ips = e.nets.map((c) => c.split('/')[0]);
  sendMagic(e.macs, [...new Set([e.pub, ...bcastList].filter(Boolean))]);   // internet route + relay on the same network
  if (!helpers.length) { sock.end('INET\n'); log('wake', id, 'over the internet only'); return; }
  for (const h of helpers) h.write(`WAKE ${e.macs.join(',')} ${bcastList.join(',') || '-'} ${ips.join(',') || '-'}\n`);
  sock.end(`SENT ${helpers.length}\n`);
  log('wake', id, 'via', helpers.length, 'from', cleanAddr(sock.remoteAddress));
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
    else if (cmd === 'WAKE') handleWake(sock, arg);
    else sock.destroy();
  });
});

server.listen(PORT, () => log(`RemoteDesk relay listening on :${PORT}`));

// In-app updates (relay/update-server.js): UPDATE_PORT=0 turns them off.
const UPDATE_PORT = process.env.UPDATE_PORT === undefined ? 21117 : Number(process.env.UPDATE_PORT);
if (UPDATE_PORT) require('./update-server').start(UPDATE_PORT);
