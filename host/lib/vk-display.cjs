/*
 * Adapted from the owner's sm64-console host tooling (same project
 * lineage) to drive the shared NN vi layer for HBLauncher Native.
 * See THIRD_PARTY.md.  Behaviour is unchanged.
 */
'use strict';
/*
 * vk-display.cjs - shared, console-free loading/validation helpers for the
 * SM64 independent NN vi layer.
 *
 * This module only reads run artifacts from disk (compatibility-module.json,
 * connection.json, shared/sdk-functions.json, display-inspection.json). It
 * never opens an RPC connection and never touches a console. vi-layer.cjs uses
 * it for both the offline plan and the explicit --execute/--close paths.
 *
 * The layer sequence is transcribed from
 *   archive/hblauncher-cpu-sdk/homebrew-launcher/vulkan-copy.cjs:42-50,204-220
 *   archive/ns2jb-archive-.../graphics/independent-layer.cjs:22-49
 * and the app-side window/layer consumer
 *   sm64-port/gpu/vi_present.c:14
 */

const fs = require('node:fs');
const path = require('node:path');

const WIDTH = 1920;
const HEIGHT = 1080;
const LAYER_SIZE = 0x2000;
const LAYER_ALIGNMENT = 4096;
const LAYER_SETTINGS_OFFSET = 0x1000;
const LAYER_NAME_OFFSET = 0x1100;
const LAYER_STATE_OFFSET = 0x500;
const LAYER_NAME = 'default';

// Exact NN vi mangled entrypoints. C1/C2 are the same implementation for these
// symbols in the reference module; both are tracked so the plan can prove it.
const VI = {
  initialize: '_ZN2nn2vi10InitializeEv',
  openDisplay: '_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE',
  layerCtor: '_ZN2nn2vi5LayerC1Ev',
  layerCtorAlias: '_ZN2nn2vi5LayerC2Ev',
  layerSettingsCtor: '_ZN2nn2vi21LayerCreationSettingsC1Eii',
  layerSettingsCtorAlias: '_ZN2nn2vi21LayerCreationSettingsC2Eii',
  layerOpen: '_ZN2nn2vi5Layer4OpenEPKNS0_21LayerCreationSettingsEPNS0_7DisplayEPKc',
  getNativeWindow: '_ZN2nn2vi5Layer15GetNativeWindowEv',
  getNativeWindowFree: '_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE',
  layerClose: '_ZN2nn2vi5Layer5CloseEb',
  layerCloseable: '_ZNK2nn2vi5Layer11IsCloseableEv',
  closeDisplay: '_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE',
  destroyLayer: '_ZN2nn2vi12DestroyLayerEPNS0_5LayerE',
  finalize: '_ZN2nn2vi8FinalizeEv',
};

// The only Vulkan function this tool resolves (requirement: no other vk*).
const VI_SURFACE = 'vkCreateViSurfaceNN';
const VI_SURFACE_STYPE = 1000062000; // VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN
const VI_SURFACE_STRUCT_SIZE = 32;
const VI_SURFACE_WINDOW_OFFSET = 24;

const SDK_NAMES = ['allocate', 'free', 'memset'];

function isFile(file) {
  try { return fs.statSync(file).isFile(); } catch { return false; }
}

function readJson(file, label) {
  let text;
  try { text = fs.readFileSync(file, 'utf8'); }
  catch (error) { throw Error('cannot read ' + (label || file) + ': ' + error.message); }
  try { return JSON.parse(text); }
  catch (error) { throw Error('malformed JSON in ' + (label || file) + ': ' + error.message); }
}

function isSafeAddress(value) {
  return Number.isSafeInteger(value) && value > 0 && value <= Number.MAX_SAFE_INTEGER;
}

// A module export must carry a section and STT_FUNC (type 2), matching the
// reference resolver in vulkan-copy.cjs:25.
function findExport(module, name) {
  return (module.symbols || []).find(s => s.name === name && s.section && s.type === 2) || null;
}

function symbolInfo(module, name) {
  const symbol = findExport(module, name);
  if (!symbol) {
    return {name, exported: false, offset: null, address: null, addressHex: null, size: null};
  }
  const address = module.base + symbol.offset;
  if (!isSafeAddress(address)) throw Error('unsafe address for ' + name);
  return {
    name,
    exported: true,
    offset: symbol.offset,
    address,
    addressHex: '0x' + address.toString(16),
    size: symbol.size,
  };
}

function requireExport(module, name, role) {
  const info = symbolInfo(module, name);
  if (!info.exported) throw Error('missing module export ' + name + (role ? ' (' + role + ')' : ''));
  return info;
}

function loadSdk(run) {
  const file = path.join(run, 'shared/sdk-functions.json');
  if (!isFile(file)) throw Error('missing shared/sdk-functions.json under ' + run);
  const table = readJson(file, 'sdk-functions');
  const entries = {};
  for (const name of SDK_NAMES) {
    const entry = table[name];
    if (!entry || !Number.isInteger(entry.offset) || typeof entry.bytes !== 'string') {
      throw Error('sdk-functions.json is missing a usable "' + name + '" entry');
    }
    if (!/^[0-9a-f]+$/i.test(entry.bytes) || entry.bytes.length % 2) {
      throw Error('sdk-functions.json "' + name + '" bytes are not even-length hex');
    }
    entries[name] = {offset: entry.offset, bytes: entry.bytes, prefix: entry.bytes.slice(0, 16)};
  }
  return {file, entries};
}

// "Current run" is the connection.json session id. Run directories are named
// after that session (switch2-<id>), so a mismatch means the artifacts and the
// live helper belong to different runs.
function validateSession(run, session, source) {
  if (!session) throw Error('run has no session id in ' + (source || 'connection.json'));
  if (typeof session !== 'string') throw Error('session id is not a string');
  const base = path.basename(run);
  if (/^switch2-/.test(base) && base !== session) {
    throw Error('session mismatch: run directory "' + base + '" != connection session "' + session + '"');
  }
  return session;
}

function loadRun(runDir) {
  if (!runDir) throw Error('usage: vi-layer.cjs <run-dir> [--execute|--close]');
  const run = path.resolve(runDir);
  if (!fs.existsSync(run)) throw Error('run directory does not exist: ' + run);

  const moduleFile = path.join(run, 'compatibility-module.json');
  if (!isFile(moduleFile)) throw Error('missing compatibility-module.json under ' + run);
  const module = readJson(moduleFile, 'compatibility-module');
  if (!Number.isSafeInteger(module.base) || module.base <= 0 || !Array.isArray(module.symbols)) {
    throw Error('malformed compatibility module: ' + moduleFile);
  }

  const connectionFile = path.join(run, 'connection.json');
  const connection = isFile(connectionFile) ? readJson(connectionFile, 'connection') : null;
  const session = connection && connection.addresses && connection.addresses.session;
  validateSession(run, session, 'connection.json');

  const sdk = loadSdk(run);

  const inspectionFile = path.join(run, 'display-inspection.json');
  const inspection = isFile(inspectionFile) ? readJson(inspectionFile, 'display-inspection') : null;
  if (inspection && inspection.session !== session) {
    throw Error('session mismatch: display-inspection.json "' + inspection.session +
      '" != connection session "' + session + '"');
  }

  return {run, session, module, moduleFile, connection, connectionFile, sdk, inspection, inspectionFile};
}

// Cross-check every resolved NN vi symbol that also appears in
// display-inspection.json. The import slot target must equal module.base+offset
// and must be marked resolved. This is a disk-only check; it never queries the
// console.
function crossCheckInspection(inspection, module, infos) {
  const checks = [];
  if (!inspection || !inspection.imports) {
    return {available: false, checks, warnings: ['display-inspection.json not present; skipped import cross-check']};
  }
  const warnings = [];
  for (const info of infos) {
    const imported = inspection.imports[info.name];
    if (!imported) continue;
    if (imported.unresolved) {
      checks.push({name: info.name, ok: false, reason: 'import is unresolved in display-inspection.json'});
      continue;
    }
    const ok = imported.target === info.address;
    checks.push({
      name: info.name, ok,
      inspectionTarget: imported.target,
      moduleAddress: info.address,
      reason: ok ? 'target == module.base + offset' : 'target differs from module.base + offset',
    });
    if (!ok) warnings.push('import target mismatch for ' + info.name);
    const region = imported.region;
    if (region && (region.permissions & 5) !== 5) {
      warnings.push(info.name + ' target region is not executable');
    }
  }
  return {available: true, checks, warnings};
}

// Offline structural validation of the required export set. Returns the
// resolved infos plus any problems. "Problems" for required symbols are fatal.
function resolveViSymbols(module) {
  const required = {
    openDisplay: VI.openDisplay,
    layerCtor: VI.layerCtor,
    layerSettingsCtor: VI.layerSettingsCtor,
    layerOpen: VI.layerOpen,
    getNativeWindow: VI.getNativeWindow,
  };
  if (!findExport(module, VI.getNativeWindow)) required.getNativeWindow = VI.getNativeWindowFree;

  const problems = [];
  const symbols = {};
  for (const [key, name] of Object.entries(required)) {
    const info = symbolInfo(module, name);
    symbols[key] = info;
    if (!info.exported) problems.push('missing module export ' + name + ' (' + key + ')');
  }

  const optionalNames = {
    layerCtorAlias: VI.layerCtorAlias,
    layerSettingsCtorAlias: VI.layerSettingsCtorAlias,
    layerClose: VI.layerClose,
    layerCloseable: VI.layerCloseable,
    closeDisplay: VI.closeDisplay,
    destroyLayer: VI.destroyLayer,
    initialize: VI.initialize,
    getNativeWindowFree: VI.getNativeWindowFree,
  };
  const optional = {};
  for (const [key, name] of Object.entries(optionalNames)) optional[key] = symbolInfo(module, name);

  // Class constructors and Open must be distinct from the plain getters; these
  // are cheap structural sanity checks on the module offsets.
  const seen = new Map();
  for (const key of ['openDisplay', 'layerCtor', 'layerSettingsCtor', 'layerOpen', 'getNativeWindow']) {
    const info = symbols[key];
    if (!info.exported) continue;
    if (info.size === 0) problems.push(info.name + ' has zero size');
    if (seen.has(info.address)) problems.push(info.name + ' aliases ' + seen.get(info.address));
    else seen.set(info.address, info.name);
  }
  for (const key of ['layerClose', 'layerCloseable']) {
    if (!optional[key].exported) problems.push('missing module export ' + optional[key].name + ' (' + key + ')');
  }
  if (optional.layerCtorAlias.exported && symbols.layerCtor.exported &&
      optional.layerCtorAlias.address !== symbols.layerCtor.address) {
    problems.push('C1/C2 layer constructors disagree: ' + optional.layerCtorAlias.addressHex +
      ' != ' + symbols.layerCtor.addressHex);
  }
  if (optional.layerSettingsCtorAlias.exported && symbols.layerSettingsCtor.exported &&
      optional.layerSettingsCtorAlias.address !== symbols.layerSettingsCtor.address) {
    problems.push('C1/C2 LayerCreationSettings constructors disagree: ' +
      optional.layerSettingsCtorAlias.addressHex + ' != ' + symbols.layerSettingsCtor.addressHex);
  }
  if (symbols.getNativeWindow.name === VI.getNativeWindow &&
      symbols.getNativeWindow.exported && symbols.getNativeWindow.size !== 8) {
    problems.push('member GetNativeWindow wrapper size is ' + symbols.getNativeWindow.size + ', expected 8');
  }
  return {symbols, optional, problems};
}

module.exports = {
  WIDTH, HEIGHT, LAYER_SIZE, LAYER_ALIGNMENT, LAYER_SETTINGS_OFFSET,
  LAYER_NAME_OFFSET, LAYER_STATE_OFFSET, LAYER_NAME,
  VI, VI_SURFACE, VI_SURFACE_STYPE, VI_SURFACE_STRUCT_SIZE, VI_SURFACE_WINDOW_OFFSET,
  SDK_NAMES,
  isFile, readJson, isSafeAddress, findExport, symbolInfo, requireExport,
  loadSdk, validateSession, loadRun, crossCheckInspection, resolveViSymbols,
};
