// Headless viewer to test a running RemoteDesk host end-to-end:
//   node viewer-test.js <hostId> <password> [relayHost] [relayPort]
// Performs the ECDH/AES-GCM handshake, logs in with the unattended password
// and checks that screen frames arrive and decode as JPEG.
'use strict';
const net = require('net');
const crypto = require('crypto');

const [id, password, rhost = '127.0.0.1', rport = 21116] = process.argv.slice(2);
const MAGIC = Buffer.from([0x52, 0x44, 0x53, 0x4b, 0x31, 0, 0, 0]);   // "RDSK1"
const T = { HELLO: 1, ACCEPT: 2, REJECT: 3, SCREEN_INFO: 4, RECT: 5, FRAME_END: 6, FRAME_ACK: 7, MOUSE_MOVE: 8,
            MONITORS: 13, SELECT_SCREEN: 14 };

const sock = net.connect(Number(rport), rhost);
let buf = Buffer.alloc(0);
let waiter = null;
sock.on('data', (d) => { buf = Buffer.concat([buf, d]); if (waiter) waiter(); });
sock.on('error', (e) => { console.error('socket error', e.message); process.exit(1); });
async function read(n) {
  while (buf.length < n) await new Promise((r) => (waiter = r));
  const out = buf.subarray(0, n); buf = buf.subarray(n); return out;
}
async function line() {
  let i;
  while ((i = buf.indexOf(10)) < 0) await new Promise((r) => (waiter = r));
  const l = buf.subarray(0, i).toString(); buf = buf.subarray(i + 1); return l;
}

(async () => {
  sock.write(`CONN ${id}\n`);
  const l = await line();
  if (l !== 'READY') throw new Error('relay said ' + l);

  const ecdh = crypto.createECDH('prime256v1');
  const pub = ecdh.generateKeys().subarray(1);                  // X||Y
  const blobHdr = Buffer.alloc(8);
  blobHdr.writeUInt32LE(0x314b4345, 0);                         // BCRYPT_ECDH_PUBLIC_P256_MAGIC
  blobHdr.writeUInt32LE(32, 4);
  const vpub = Buffer.concat([blobHdr, pub]);
  sock.write(Buffer.concat([MAGIC, vpub]));
  const peer = await read(80);
  if (!peer.subarray(0, 8).equals(MAGIC)) throw new Error('bad magic');
  const hpub = peer.subarray(8);
  const secret = ecdh.computeSecret(Buffer.concat([Buffer.from([4]), hpub.subarray(8)]));
  // CNG's BCRYPT_KDF_HASH hashes the secret in big-endian byte order.
  const kdf = (label) => crypto.createHash('sha256').update(Buffer.concat([Buffer.from(label), vpub, hpub, secret])).digest();
  const tx = kdf('v2h'), rx = kdf('h2v');
  const code = kdf('code').subarray(0, 4).toString('hex');
  let txc = 0n, rxc = 0n;
  const nonce = (c) => { const n = Buffer.alloc(12); n.writeBigUInt64LE(c, 4); return n; };

  const send = (type, payload = Buffer.alloc(0)) => {
    const c = crypto.createCipheriv('aes-256-gcm', tx, nonce(txc++));
    const ct = Buffer.concat([c.update(Buffer.concat([Buffer.from([type]), payload])), c.final()]);
    const len = Buffer.alloc(4); len.writeUInt32LE(ct.length);
    sock.write(Buffer.concat([len, c.getAuthTag(), ct]));
  };
  const recv = async () => {
    const len = (await read(4)).readUInt32LE();
    const tag = await read(16);
    const ct = await read(len);
    const d = crypto.createDecipheriv('aes-256-gcm', rx, nonce(rxc++));
    d.setAuthTag(tag);
    const pt = Buffer.concat([d.update(ct), d.final()]);       // throws if keys mismatch
    return { type: pt[0], data: pt.subarray(1) };
  };

  send(T.HELLO, Buffer.from(`000000000\n${password}`));
  const r = await recv();
  if (r.type !== T.ACCEPT) throw new Error('not accepted: ' + r.data.toString());
  console.log(`accepted, security code ${code.slice(0, 4)}-${code.slice(4)}`);

  let frames = 0, rects = 0, bytes = 0, w = 0, h = 0;
  let monitors = [], selected = null;
  const t0 = Date.now();
  while (frames < 20 && Date.now() - t0 < 15000) {
    const m = await recv();
    if (m.type === T.SCREEN_INFO) { w = m.data.readInt32LE(0); h = m.data.readInt32LE(4); }
    else if (m.type === T.MONITORS) {
      selected = m.data.readInt32LE(0);
      monitors = [];
      for (let o = 4; o + 17 <= m.data.length; o += 17)
        monitors.push({ x: m.data.readInt32LE(o), y: m.data.readInt32LE(o + 4), w: m.data.readInt32LE(o + 8),
                        h: m.data.readInt32LE(o + 12), primary: !!m.data[o + 16] });
    }
    else if (m.type === T.RECT) {
      if (m.data[16] !== 0xff || m.data[17] !== 0xd8) throw new Error('rect is not JPEG');
      rects++; bytes += m.data.length;
    } else if (m.type === T.FRAME_END) {
      frames++;
      send(T.FRAME_ACK);
      const mv = Buffer.alloc(8); mv.writeInt32LE(100 + frames * 10, 0); mv.writeInt32LE(300, 4);
      send(T.MOUSE_MOVE, mv);                                    // moves host cursor -> new frames
    }
  }
  console.log(`screen ${w}x${h}, ${frames} frames, ${rects} JPEG rects, ${(bytes / 1024).toFixed(0)} KiB`);
  if (!w || frames < 2) throw new Error('not enough frames');
  console.log(`monitors: ${monitors.map((m) => `${m.w}x${m.h}@${m.x},${m.y}${m.primary ? '*' : ''}`).join(' ')}, showing ${selected}`);

  // Switch to each screen, then all screens, and check the host follows.
  if (monitors.length > 1) {
    const allW = Math.max(...monitors.map((m) => m.x + m.w)) - Math.min(...monitors.map((m) => m.x));
    const allH = Math.max(...monitors.map((m) => m.y + m.h)) - Math.min(...monitors.map((m) => m.y));
    for (const want of [...monitors.keys(), -1]) {
      const sel = Buffer.alloc(4); sel.writeInt32LE(want);
      send(T.SELECT_SCREEN, sel);
      let got = null, size = null, gotFrame = false;
      const t1 = Date.now();
      while (!(got === want && size && gotFrame) && Date.now() - t1 < 5000) {
        const m = await recv();
        if (m.type === T.MONITORS) got = m.data.readInt32LE(0);
        else if (m.type === T.SCREEN_INFO && got === want) size = [m.data.readInt32LE(0), m.data.readInt32LE(4)];
        else if (m.type === T.FRAME_END) { send(T.FRAME_ACK); if (size) gotFrame = true; }
      }
      const exp = want < 0 ? [allW, allH] : [monitors[want].w, monitors[want].h];
      if (got !== want || !size || size[0] !== exp[0] || size[1] !== exp[1] || !gotFrame)
        throw new Error(`switch to screen ${want}: got ${got} ${size}, expected ${exp}`);
      console.log(`switched to ${want < 0 ? 'all screens' : 'screen ' + (want + 1)}: ${size[0]}x${size[1]}, frames OK`);
    }
  }
  console.log('host session test: PASSED');
  sock.destroy();
})().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
