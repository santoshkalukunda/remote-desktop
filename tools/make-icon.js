// Generates src/app.ico (16..256 px PNG entries): an emerald rounded square with a
// white monitor and a connection arrow, plus the MSIX / Store logos in
// packaging/Assets. Pure Node, no dependencies.
//   node tools/make-icon.js
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

// ---- shapes in unit coordinates (0..1) ----
const inRoundRect = (x, y, x0, y0, x1, y1, r) => {
  if (x < x0 || x > x1 || y < y0 || y > y1) return false;
  const cx = Math.min(Math.max(x, x0 + r), x1 - r);
  const cy = Math.min(Math.max(y, y0 + r), y1 - r);
  return (x - cx) ** 2 + (y - cy) ** 2 <= r * r;
};
const inRect = (x, y, x0, y0, w, h) => x >= x0 && x <= x0 + w && y >= y0 && y <= y0 + h;
const inPoly = (x, y, pts) => {
  let inside = false;
  for (let i = 0, j = pts.length - 1; i < pts.length; j = i++) {
    const [xi, yi] = pts[i], [xj, yj] = pts[j];
    if ((yi > y) !== (yj > y) && x < ((xj - xi) * (y - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
};
const ARROW = [[.36, .39], [.52, .39], [.52, .33], [.65, .43], [.52, .53], [.52, .47], [.36, .47]];

// Colour of one sample point, or null if transparent.
function sample(x, y) {
  if (!inRoundRect(x, y, .02, .02, .98, .98, .22)) return null;
  const white = [255, 255, 255];
  const screen = [6, 95, 70];    // deep emerald (#065F46)
  if (inPoly(x, y, ARROW)) return white;
  if (inRect(x, y, .23, .27, .54, .32)) return screen;
  if (inRect(x, y, .18, .22, .64, .42) || inRect(x, y, .45, .64, .10, .09) || inRect(x, y, .32, .72, .36, .06))
    return white;
  const t = (x + y) / 2;   // diagonal gradient
  return [52 + (4 - 52) * t, 211 + (120 - 211) * t, 153 + (87 - 153) * t];   // #34D399 -> #047857, all emerald
}

function render(size) {
  const SS = 4;   // 4x4 supersampling for anti-aliasing
  const px = Buffer.alloc(size * size * 4);
  for (let py = 0; py < size; py++)
    for (let pxi = 0; pxi < size; pxi++) {
      let r = 0, g = 0, b = 0, a = 0;
      for (let sy = 0; sy < SS; sy++)
        for (let sx = 0; sx < SS; sx++) {
          const c = sample((pxi + (sx + .5) / SS) / size, (py + (sy + .5) / SS) / size);
          if (c) { r += c[0]; g += c[1]; b += c[2]; a++; }
        }
      const o = (py * size + pxi) * 4;
      if (a) { px[o] = r / a; px[o + 1] = g / a; px[o + 2] = b / a; px[o + 3] = (255 * a) / (SS * SS); }
    }
  return png(size, px);
}

// ---- minimal PNG encoder ----
const CRC = new Int32Array(256).map((_, n) => {
  let c = n;
  for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  return c;
});
const crc32 = (buf) => {
  let c = -1;
  for (const b of buf) c = CRC[(c ^ b) & 255] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
};
function chunk(type, data) {
  const len = Buffer.alloc(4); len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type), data]);
  const crc = Buffer.alloc(4); crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}
function png(size, rgba) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(size, 0); ihdr.writeUInt32BE(size, 4);
  ihdr[8] = 8; ihdr[9] = 6;   // 8-bit RGBA
  const raw = Buffer.alloc(size * (size * 4 + 1));
  for (let y = 0; y < size; y++) rgba.copy(raw, y * (size * 4 + 1) + 1, y * size * 4, (y + 1) * size * 4);
  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw, { level: 9 })), chunk('IEND', Buffer.alloc(0)),
  ]);
}

// ---- ICO container ----
const sizes = [16, 24, 32, 48, 64, 128, 256];
const images = sizes.map(render);
const header = Buffer.alloc(6 + 16 * sizes.length);
header.writeUInt16LE(0, 0); header.writeUInt16LE(1, 2); header.writeUInt16LE(sizes.length, 4);
let offset = header.length;
sizes.forEach((s, i) => {
  const e = 6 + 16 * i;
  header[e] = s % 256; header[e + 1] = s % 256;   // 256 is stored as 0
  header.writeUInt16LE(1, e + 4); header.writeUInt16LE(32, e + 6);
  header.writeUInt32LE(images[i].length, e + 8); header.writeUInt32LE(offset, e + 12);
  offset += images[i].length;
});
const out = path.join(__dirname, '..', 'src', 'app.ico');
fs.writeFileSync(out, Buffer.concat([header, ...images]));
fs.mkdirSync(path.join(__dirname, '..', 'build'), { recursive: true });
fs.writeFileSync(path.join(__dirname, '..', 'build', 'icon-preview.png'), images[images.length - 1]);
console.log('wrote', out);

// ---- MSIX logos (names referenced by packaging/AppxManifest.xml) ----
const assets = path.join(__dirname, '..', 'packaging', 'Assets');
fs.mkdirSync(assets, { recursive: true });
for (const [name, size] of [['Square44x44Logo', 44], ['Square150x150Logo', 150], ['StoreLogo', 50]]) {
  fs.writeFileSync(path.join(assets, name + '.png'), render(size));
}
console.log('wrote', assets);
