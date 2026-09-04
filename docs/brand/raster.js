const { Resvg } = require('@resvg/resvg-js');
const fs = require('fs'), path = require('path');
const SRC = 'dist/svg', OUT = 'dist/png';
fs.mkdirSync(OUT, { recursive: true });

const read = f => fs.readFileSync(path.join(SRC, f), 'utf8');
const ink  = (svg, c) => svg.replace(/currentColor/g, c);

function png(svg, w, name) {
  const r = new Resvg(svg, { fitTo: { mode: 'width', value: w } });
  fs.writeFileSync(path.join(OUT, name), r.render().asPng());
  return name;
}

const mark = read('didi-mark.svg');
const out = [];
// square marks — transparent background, three ink treatments
for (const s of [16, 20, 24, 32, 48, 64, 128, 256, 512, 1024]) {
  out.push(png(read('didi-mark-gradient.svg'), s, `didi-mark-gradient-${s}.png`));
  out.push(png(ink(mark, '#12151B'), s, `didi-mark-ink-${s}.png`));
  out.push(png(ink(mark, '#F6F7F9'), s, `didi-mark-white-${s}.png`));
}
// app icon tile
for (const s of [16, 32, 64, 128, 256, 512, 1024])
  out.push(png(read('didi-icon-rounded.svg'), s, `didi-icon-${s}.png`));
// favicons use the compact weight — extra mass survives 16px
for (const s of [16, 32, 48])
  out.push(png(read('favicon.svg'), s, `favicon-${s}.png`));
// lockups
for (const [f, c, n] of [
  ['didi-signature.svg', '#F6F7F9', 'didi-signature-white'],
  ['didi-signature.svg', '#12151B', 'didi-signature-ink'],
  ['didi-signature-brand.svg', '#F6F7F9', 'didi-signature-brand-dark'],
  ['didi-signature-brand.svg', '#12151B', 'didi-signature-brand-light'],
]) out.push(png(ink(read(f), c), 1600, `${n}-1600.png`));
out.push(png(ink(read('didi-lockup-stacked.svg'), '#F6F7F9'), 900, 'didi-stacked-white-900.png'));
out.push(png(ink(read('didi-lockup-stacked.svg'), '#12151B'), 900, 'didi-stacked-ink-900.png'));
for (const [f, w] of [['social-preview', 1280], ['readme-banner', 1280]])
  out.push(png(read(f + '.svg'), w, f + '.png'));
console.log(`${out.length} PNGs written`);
