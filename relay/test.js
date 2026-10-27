// End-to-end check of the relay protocol: node test.js (starts its own relay).
'use strict';
const net = require('net');
const { spawn } = require('child_process');
const assert = require('assert');

const PORT = 21999;
const relay = spawn(process.execPath, [__dirname + '/server.js'], { env: { ...process.env, PORT } });

function lines(sock) {
  let buf = '';
  const waiters = [];
  sock.on('data', (d) => {
    buf += d.toString('latin1');
    let i;
    while (waiters.length && (i = buf.indexOf('\n')) >= 0) {
      const l = buf.slice(0, i); buf = buf.slice(i + 1); waiters.shift()(l);
    }
  });
  return { next: () => new Promise((r) => {
    const i = buf.indexOf('\n');
    if (i >= 0) { const l = buf.slice(0, i); buf = buf.slice(i + 1); r(l); } else waiters.push(r);
  }), rest: () => buf };
}
const connect = () => new Promise((r) => { const s = net.connect(PORT, '127.0.0.1', () => r(s)); });

(async () => {
  await new Promise((r) => relay.stdout.once('data', r));

  const ctrl = await connect(); const cl = lines(ctrl);
  ctrl.write('REG 123456789\n');
  assert.strictEqual(await cl.next(), 'OK');

  const dup = await connect(); const dl = lines(dup);
  dup.write('REG 123456789\n');
  assert.strictEqual(await dl.next(), 'TAKEN');

  const off = await connect(); const ol = lines(off);
  off.write('CONN 987654321\n');
  assert.strictEqual(await ol.next(), 'OFFLINE');

  ctrl.write('PING\n');
  assert.strictEqual(await cl.next(), 'PONG');

  const viewer = await connect(); const vl = lines(viewer);
  viewer.write('CONN 123456789\n');
  const call = await cl.next();
  assert.match(call, /^CALL [0-9a-f]{32}$/);

  const join = await connect(); const jl = lines(join);
  join.write(`JOIN ${call.slice(5)}\n`);
  assert.strictEqual(await vl.next(), 'READY');
  assert.strictEqual(await jl.next(), 'READY');

  // raw bytes flow both ways after READY
  const echo = (sock) => new Promise((r) => sock.once('data', (d) => r(d.toString())));
  let p = echo(join); viewer.write('hello-host'); assert.strictEqual(await p, 'hello-host');
  p = echo(viewer); join.write('hello-viewer'); assert.strictEqual(await p, 'hello-viewer');

  // closing one side closes the other
  const closed = new Promise((r) => join.once('close', r));
  viewer.destroy();
  await closed;

  console.log('relay test: all checks passed');
  relay.kill();
  process.exit(0);
})().catch((e) => { console.error(e); relay.kill(); process.exit(1); });
