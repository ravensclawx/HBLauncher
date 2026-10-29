'use strict';
/*
 * packages.cjs - discover and validate `apps/<id>` packages.
 *
 * A package is a directory with manifest.json, a built code.bin (plus optional
 * data.bin) and an optional icon.png.  The loader never trusts the manifest
 * blindly: ids, kinds, ABI versions, file sizes and hash presence are checked
 * before anything is mapped.
 */
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const {check, sha256, readJson} = require('./common.cjs');

const ABIS = new Set([3]);
const KINDS = new Set(['native', 'nes', 'sm64']);
const ID_PATTERN = /^[a-z0-9][a-z0-9._-]{0,63}$/;

function parseVersion(value) {
  const match = /^(\d{1,5})\.(\d{1,5})\.(\d{1,5})$/.exec(String(value));
  if (!match) return null;
  const [, major, minor, patch] = match.map(Number);
  if (major > 0xffff || minor > 0xff || patch > 0xff) return null;
  return {major, minor, patch};
}

/* --- Minimal PNG decoder (8-bit, non-interlaced, colour types 0/2/4/6) --- */
function decodePng(buffer) {
  const signature = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]);
  check(buffer.length > 33 && buffer.subarray(0, 8).equals(signature), 'not a PNG file', 'png');
  let offset = 8;
  let width = 0, height = 0, depth = 0, colorType = 0, interlace = 0;
  const idat = [];
  let palette = null;
  while (offset + 8 <= buffer.length) {
    const length = buffer.readUInt32BE(offset);
    const type = buffer.subarray(offset + 4, offset + 8).toString('ascii');
    const data = buffer.subarray(offset + 8, offset + 8 + length);
    offset += 12 + length;
    if (type === 'IHDR') {
      width = data.readUInt32BE(0);
      height = data.readUInt32BE(4);
      depth = data[8];
      colorType = data[9];
      interlace = data[12];
    } else if (type === 'PLTE') {
      palette = data;
    } else if (type === 'IDAT') {
      idat.push(data);
    } else if (type === 'IEND') {
      break;
    }
  }
  check(width > 0 && height > 0, 'PNG has no size', 'png');
  check(width <= 4096 && height <= 4096, 'PNG is too large', 'png');
  check(depth === 8 && interlace === 0, 'only 8-bit non-interlaced PNG icons are supported', 'png');
  check([0, 2, 4, 6].includes(colorType), 'unsupported PNG colour type', 'png');
  const channels = {0: 1, 2: 3, 4: 2, 6: 4}[colorType];
  const raw = zlib.inflateSync(Buffer.concat(idat));
  const stride = width * channels;
  const pixels = Buffer.alloc(stride * height);
  const bpp = channels;
  for (let y = 0; y < height; y++) {
    const filter = raw[y * (stride + 1)];
    const line = raw.subarray(y * (stride + 1) + 1, (y + 1) * (stride + 1));
    const out = pixels.subarray(y * stride, (y + 1) * stride);
    const prior = y ? pixels.subarray((y - 1) * stride, y * stride) : Buffer.alloc(stride);
    for (let x = 0; x < stride; x++) {
      const a = x >= bpp ? out[x - bpp] : 0;
      const b = prior[x];
      const c = x >= bpp ? prior[x - bpp] : 0;
      let value = line[x];
      if (filter === 1) value += a;
      else if (filter === 2) value += b;
      else if (filter === 3) value += (a + b) >> 1;
      else if (filter === 4) {
        const p = a + b - c;
        const pa = Math.abs(p - a), pb = Math.abs(p - b), pc = Math.abs(p - c);
        value += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
      } else if (filter !== 0) throw new Error('bad PNG filter ' + filter);
      out[x] = value & 0xff;
    }
  }
  /* Expand to RGBA8. */
  const rgba = Buffer.alloc(width * height * 4);
  for (let i = 0; i < width * height; i++) {
    let r, g, b, a = 255;
    if (colorType === 6) { r = pixels[i * 4]; g = pixels[i * 4 + 1]; b = pixels[i * 4 + 2]; a = pixels[i * 4 + 3]; }
    else if (colorType === 2) { r = pixels[i * 3]; g = pixels[i * 3 + 1]; b = pixels[i * 3 + 2]; }
    else if (colorType === 0) { r = g = b = pixels[i]; }
    else { r = g = b = pixels[i * 2]; a = pixels[i * 2 + 1]; }
    rgba[i * 4] = r; rgba[i * 4 + 1] = g; rgba[i * 4 + 2] = b; rgba[i * 4 + 3] = a;
  }
  return {width, height, rgba, palette};
}

/* Nearest-neighbour resize to 96x96 RGBA (icons are flat art; the console
   samples them with a linear filter anyway). */
function resizeIcon(image, size = 96) {
  const out = Buffer.alloc(size * size * 4);
  for (let y = 0; y < size; y++) {
    const sy = Math.min(image.height - 1, Math.floor(y * image.height / size));
    for (let x = 0; x < size; x++) {
      const sx = Math.min(image.width - 1, Math.floor(x * image.width / size));
      const from = (sy * image.width + sx) * 4;
      const to = (y * size + x) * 4;
      out[to] = image.rgba[from];
      out[to + 1] = image.rgba[from + 1];
      out[to + 2] = image.rgba[from + 2];
      out[to + 3] = image.rgba[from + 3];
    }
  }
  return out;
}

function loadPackage(directory) {
  const manifestFile = path.join(directory, 'manifest.json');
  check(fs.existsSync(manifestFile), `missing ${manifestFile}`, 'package');
  const manifest = readJson(manifestFile);
  const problems = [];
  if (!ID_PATTERN.test(manifest.id || '')) problems.push('invalid id');
  if ((manifest.assets || []).length > 64) problems.push('at most 64 assets');
  if (!manifest.name || typeof manifest.name !== 'string') problems.push('missing name');
  if (!parseVersion(manifest.version)) problems.push('version must be x.y.z');
  if (!ABIS.has(manifest.abi)) problems.push(`unsupported abi ${manifest.abi}`);
  if (!KINDS.has(manifest.kind)) problems.push(`unsupported kind ${manifest.kind}`);
  check(problems.length === 0, `${manifest.id || directory}: ${problems.join(', ')}`, 'package');

  let report = null;
  let code = Buffer.alloc(0);
  let data = Buffer.alloc(0);
  if (manifest.kind === 'native') {
    const reportFile = path.join(directory, 'build', 'report.json');
    const codeFile = path.join(directory, 'build', 'code.bin');
    const dataFile = path.join(directory, 'build', 'data.bin');
    check(fs.existsSync(reportFile), `${manifest.id}: build/report.json missing (run sdk/tools/hbx-build.py)`, 'package');
    check(fs.existsSync(codeFile), `${manifest.id}: build/code.bin missing`, 'package');
    report = readJson(reportFile);
    code = fs.readFileSync(codeFile);
    check(report.codeSha256 === sha256(code), `${manifest.id}: code.bin does not match its report`, 'package');
    check(report.codeBytes === code.length, `${manifest.id}: code size mismatch`, 'package');
    check(report.abi === manifest.abi, `${manifest.id}: manifest/report ABI mismatch`, 'package');
    const codeBase = Number(BigInt(report.codeBase));
    const dataBase = Number(BigInt(report.dataBase));
    const entry = Number(BigInt(report.entry));
    if (codeBase % 0x1000 || dataBase % 0x1000)
      problems.push('code/data bases must be page aligned');
    if (codeBase === dataBase) problems.push('code and data bases must differ');
    if (!(entry >= codeBase && entry < codeBase + code.length))
      problems.push('entry is outside the code image');
    if (!Number.isInteger(report.dataRegionBytes) || report.dataRegionBytes < 0)
      problems.push('dataRegionBytes is invalid');
    data = fs.existsSync(dataFile) ? fs.readFileSync(dataFile) : Buffer.alloc(0);
    if (data.length > (report.dataRegionBytes || 0))
      problems.push('data.bin does not fit its data region');
  } else {
    check(manifest.driver && typeof manifest.driver === 'object',
      `${manifest.id}: compatibility packages need a "driver" block (see docs/COMPAT.md)`, 'package');
  }

  /* Assets: ordered, de-duplicated, path-safe.  A "*.ext" entry expands to
     every matching file directly in assets/ (sorted; may match nothing), so a
     user can drop in content such as ROMs without editing the manifest. */
  const assets = [];
  const seen = new Set();
  for (const entry of manifest.assets || []) {
    if (typeof entry !== 'string' || entry.includes('..') || path.isAbsolute(entry))
      throw new Error(`${manifest.id}: unsafe asset path ${entry}`);
    let names = [entry];
    const glob = /^\*(\.[A-Za-z0-9]+)$/.exec(entry);
    if (glob) {
      const folder = path.join(directory, 'assets');
      const suffix = glob[1].toLowerCase();
      names = fs.existsSync(folder) ? fs.readdirSync(folder)
        .filter(name => name.toLowerCase().endsWith(suffix) &&
          fs.statSync(path.join(folder, name)).isFile())
        .sort((a, b) => a.localeCompare(b)) : [];
    }
    for (const name of names) {
      if (seen.has(name)) continue;
      seen.add(name);
      const file = path.join(directory, 'assets', name);
      if (!fs.existsSync(file)) throw new Error(`${manifest.id}: asset ${name} missing`);
      assets.push({name, bytes: fs.readFileSync(file)});
    }
  }
  check(assets.length <= 64, `${manifest.id}: at most 64 assets (found ${assets.length})`, 'package');

  let icon = null;
  const iconName = manifest.icon || 'icon.png';
  if (typeof iconName !== 'string' || iconName.includes('..') || path.isAbsolute(iconName)) {
    problems.push('icon path must be a relative file name');
  }
  const iconFile = path.join(directory, iconName);
  if (fs.existsSync(iconFile)) {
    try {
      icon = resizeIcon(decodePng(fs.readFileSync(iconFile)));
    } catch (error) {
      problems.push(`icon: ${error.message}`);
    }
  }

  const limits = Object.assign({
    heap_bytes: 4 * 1024 * 1024,
    stack_bytes: 1024 * 1024,
    frames: 3600
  }, manifest.limits || {});
  for (const key of ['heap_bytes', 'stack_bytes', 'frames']) {
    if (!Number.isInteger(limits[key]) || limits[key] < 0)
      check(false, `${manifest.id}: limits.${key} must be a non-negative integer`, 'package');
  }
  check(limits.heap_bytes >= 65536 && limits.heap_bytes <= 64 * 1024 * 1024,
    `${manifest.id}: heap limit out of range`, 'package');
  check(limits.stack_bytes >= 65536 && limits.stack_bytes <= 8 * 1024 * 1024,
    `${manifest.id}: stack limit out of range`, 'package');
  check(limits.frames <= 1000000, `${manifest.id}: frame limit out of range`, 'package');

  const saveBytes = manifest.save_bytes === undefined ? 0 : manifest.save_bytes;
  check(Number.isInteger(saveBytes) && saveBytes >= 0 && saveBytes <= 16 * 1024 * 1024,
    `${manifest.id}: save_bytes must be an integer up to 16 MiB`, 'package');

  const imports = manifest.imports === undefined ? [] : manifest.imports;
  check(Array.isArray(imports) && imports.length <= 4096 &&
    imports.every(name => typeof name === 'string' && /^[A-Za-z_][A-Za-z0-9_.$]*$/.test(name)),
    `${manifest.id}: imports must be a list of SDK symbol names`, 'package');

  return {
    directory,
    manifest,
    saveBytes,
    imports,
    report,
    code,
    data,
    assets,
    icon,
    limits,
    idHash: fnv1a(manifest.id),
    version: parseVersion(manifest.version),
    warnings: problems
  };
}

function fnv1a(text) {
  let hash = 2166136261 >>> 0;
  for (const byte of Buffer.from(text, 'utf8')) {
    hash ^= byte;
    hash = Math.imul(hash, 16777619) >>> 0;
  }
  return hash >>> 0;
}

function scan(appsDirectory, log) {
  if (!fs.existsSync(appsDirectory)) return {packages: [], errors: []};
  const packages = [];
  const errors = [];
  for (const entry of fs.readdirSync(appsDirectory).sort()) {
    const directory = path.join(appsDirectory, entry);
    if (!fs.statSync(directory).isDirectory()) continue;
    if (!fs.existsSync(path.join(directory, 'manifest.json'))) continue;
    try {
      const loaded = loadPackage(directory);
      packages.push(loaded);
      if (log) log.info(`app ${loaded.manifest.id} ${loaded.manifest.version} (${loaded.manifest.kind})`);
    } catch (error) {
      errors.push({directory, message: error.message});
      if (log) log.error(`app ${entry} rejected: ${error.message}`);
    }
  }
  /* Duplicate ids would collide in the catalog hash. */
  const seen = new Set();
  for (const item of [...packages]) {
    if (seen.has(item.manifest.id)) {
      errors.push({directory: item.directory, message: 'duplicate id ' + item.manifest.id});
      packages.splice(packages.indexOf(item), 1);
    } else {
      seen.add(item.manifest.id);
    }
  }
  if (log) for (const error of errors)
    log.error(`app ${path.basename(error.directory)} rejected: ${error.message}`);

  /* Stable order: kind (native, nes, sm64), then display name. */
  const order = {native: 0, nes: 1, sm64: 2};
  packages.sort((a, b) => (order[a.manifest.kind] - order[b.manifest.kind]) ||
    a.manifest.name.localeCompare(b.manifest.name));
  return {packages, errors};
}

/* Build the catalog block the launcher reads (struct hbx_catalog + strings). */
function buildCatalog(packages, options = {}) {
  const maxEntries = options.maxEntries || 64;
  const list = packages.slice(0, maxEntries);
  const strings = [];
  let stringsBytes = 0;
  const entries = [];
  let iconLayer = 0;
  const iconLayers = [];
  for (const item of list) {
    const nameOffset = stringsBytes;
    strings.push(Buffer.from(item.manifest.name + '\0', 'utf8'));
    stringsBytes += Buffer.byteLength(item.manifest.name) + 1;
    const descOffset = stringsBytes;
    strings.push(Buffer.from((item.manifest.description || '') + '\0', 'utf8'));
    stringsBytes += Buffer.byteLength(item.manifest.description || '') + 1;
    const hasIcon = !!item.icon && iconLayer < (options.maxIcons || 16);
    if (hasIcon) iconLayers.push(item.icon);
    entries.push({
      id_hash: item.idHash,
      name_off: nameOffset,
      desc_off: descOffset,
      kind: {native: 0, nes: 1, sm64: 2}[item.manifest.kind],
      version: (item.version.major << 16) | (item.version.minor << 8) | item.version.patch,
      flags: 0,
      icon_layer: hasIcon ? iconLayer : 0xffffffff,
      abi: item.manifest.abi
    });
    if (hasIcon) iconLayer++;
  }
  const headerBytes = 32 + entries.length * 32;
  const size = Math.max(headerBytes + stringsBytes, 4096);
  const block = Buffer.alloc(size);
  block.writeUInt32LE((options.magic || 0x47415443) >>> 0, 0);
  block.writeUInt32LE(entries.length, 4);
  block.writeUInt32LE(headerBytes, 8);
  block.writeUInt32LE(stringsBytes, 12);
  block.writeUInt32LE(iconLayers.length, 16);
  block.writeUInt32LE(iconLayers.length ? 1 : 0, 20);
  entries.forEach((entry, index) => {
    const at = 32 + index * 32;
    block.writeUInt32LE(entry.id_hash >>> 0, at);
    block.writeUInt32LE(entry.name_off, at + 4);
    block.writeUInt32LE(entry.desc_off, at + 8);
    block.writeUInt32LE(entry.kind, at + 12);
    block.writeUInt32LE(entry.version, at + 16);
    block.writeUInt32LE(entry.flags, at + 20);
    block.writeUInt32LE(entry.icon_layer >>> 0, at + 24);
    block.writeUInt32LE(entry.abi, at + 28);
  });
  Buffer.concat(strings).copy(block, headerBytes);
  const icons = iconLayers.length ? Buffer.concat(iconLayers) : Buffer.alloc(0);
  return {block, icons, iconLayers: iconLayers.length, entries};
}

module.exports = {scan, loadPackage, buildCatalog, decodePng, resizeIcon, fnv1a};
