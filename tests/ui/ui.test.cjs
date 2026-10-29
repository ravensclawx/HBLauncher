'use strict';
/* UI regression: render the shipped frame with real Vulkan (lavapipe) and
 * check geometry budget, animation and the offscreen image. */
const fs = require('node:fs');
const path = require('node:path');
const {execFileSync} = require('node:child_process');
const assert = require('node:assert');

const ROOT = path.resolve(__dirname, '..', '..');
const BIN = path.join(ROOT, 'build/preview/hbl-preview');
const OUT = path.join(ROOT, 'tests/preview/out');
const FRAMES = 6;

if (!fs.existsSync(BIN)) {
  console.error('build/preview/hbl-preview missing; run tests/preview/build.sh');
  process.exit(2);
}
fs.mkdirSync(OUT, {recursive: true});
const env = Object.assign({}, process.env, {
  VK_ICD_FILENAMES: process.env.VK_ICD_FILENAMES || '/usr/share/vulkan/icd.d/lvp_icd.json'
});
const stdout = execFileSync(BIN, [OUT, String(FRAMES)], {env, encoding: 'utf8'});
const lines = stdout.split('\n').filter(line => line.startsWith('frame '));
assert.strictEqual(lines.length, FRAMES, 'all frames rendered');
const counts = [];
for (const line of lines) {
  const match = /^frame (\d+): (\d+) vertices( \(OVERFLOW\))?$/.exec(line);
  assert(match, 'unexpected frame line: ' + line);
  assert(!match[3], 'vertex budget overflow on frame ' + match[1]);
  counts.push(Number(match[2]));
  assert(Number(match[2]) > 40000, 'frame has too little geometry');
  assert(Number(match[2]) <= 163840, 'frame exceeds the vertex budget');
}
assert.notStrictEqual(counts[0], counts[3], 'animation and modal change the geometry');

const files = fs.readdirSync(OUT).filter(name => name.endsWith('.ppm')).sort();
assert(files.length >= FRAMES);
for (const file of files) {
  const header = fs.readFileSync(path.join(OUT, file)).subarray(0, 32).toString();
  assert(header.startsWith('P6\n1920 1080\n255\n'), file + ' is not a 1920x1080 PPM');
}
const first = fs.readFileSync(path.join(OUT, 'frame-00.ppm'));
const second = fs.readFileSync(path.join(OUT, 'frame-01.ppm'));
assert(!first.equals(second), 'consecutive frames must differ (waves animate)');
/* The library frame must be mostly light: the paper background dominates. */
let bright = 0;
const dataStart = first.indexOf('255\n') + 4;
for (let i = dataStart; i < first.length; i += 3 * 97) if (first[i] > 200) bright++;
assert(bright > 100, 'library frame should be predominantly light');
console.log(`ui: ${FRAMES} frames, ${Math.min(...counts)}..${Math.max(...counts)} vertices, ok`);
