/*
 * Adapted from the owner's sm64-console host tooling (same project
 * lineage) to drive the shared NN vi layer for HBLauncher Native.
 * See THIRD_PARTY.md.  Behaviour is unchanged.
 */
'use strict';
/*
 * vi-layer.cjs - create and retain a 1920x1080 NN vi layer for the SM64
 * presenter without creating any Vulkan object.
 *
 * Reference sequence (transcribed, not improvised):
 *   archive/hblauncher-cpu-sdk/homebrew-launcher/vulkan-copy.cjs:44-50
 *     sdk.allocate(out, 0x1000, 0x2000) -> sdk.memset(layer, 0, 0x2000)
 *     OpenDefaultDisplay(&display)
 *     Layer::Layer(layer)
 *     LayerCreationSettings(1920, 1080) at layer+0x1000
 *     write "default" at layer+0x1100
 *     Layer::Open(settings, display, "default")
 *   archive/ns2jb-archive-.../graphics/independent-layer.cjs:22-38
 *     open-state check at layer+0x500 and Layer::GetNativeWindow()
 *   archive/hblauncher-cpu-sdk/homebrew-launcher/vulkan-copy.cjs:217-219 (close)
 *     Layer::Close(layer, 0) -> verify layer+0x500 == 00 00 -> sdk.free(layer)
 *
 * The app consumes these handles through
 *   sm64-port/gpu/vi_present.c:14 (window + layer, then
 *   vkCreateViSurfaceNN). This tool emits the absolute address of the module's
 *   vkCreateViSurfaceNN export for that context. No other Vulkan function is
 *   resolved, and no instance/device/swapchain is ever created.
 *
 * Usage:
 *   vi-layer.cjs <run-dir>              dry plan (default, offline)
 *   vi-layer.cjs <run-dir> --execute    allocate/construct/open/retain layer
 *   vi-layer.cjs <run-dir> --close      Layer::Close + free (safety-checked)
 *
 * Options: --force, --close-display, --out <file>, --quiet, --help
 * Exit codes: 0 ok, 1 validation/execute error, 2 usage error.
 */

const fs = require('node:fs');
const path = require('node:path');
const display = require('./vk-display.cjs');

const {
  WIDTH, HEIGHT, LAYER_SIZE, LAYER_ALIGNMENT, LAYER_SETTINGS_OFFSET,
  LAYER_NAME_OFFSET, LAYER_STATE_OFFSET, LAYER_NAME,
  VI, VI_SURFACE,
} = display;

const FORMAT = 'sm64-console-vi-layer/1';
const PLAN_FORMAT = 'sm64-console-vi-layer-plan/1';
const CLOSE_FORMAT = 'sm64-console-vi-layer-close/1';
const LAYER_FILE = 'vi-layer.json';
const CLOSE_FILE = 'vi-layer-close.json';

function usage(message) {
  if (message) console.error('vi-layer: ' + message);
  console.error('usage: vi-layer.cjs <run-dir> [--execute | --close] [--force] ' +
    '[--close-display] [--out <file>] [--quiet]');
  process.exit(2);
}

function parseArgs(argv) {
  const opts = {run: null, execute: false, close: false, force: false,
    closeDisplay: false, out: null, quiet: false};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--execute') opts.execute = true;
    else if (arg === '--close') opts.close = true;
    else if (arg === '--force') opts.force = true;
    else if (arg === '--close-display') opts.closeDisplay = true;
    else if (arg === '--quiet') opts.quiet = true;
    else if (arg === '--out') {
      if (++i >= argv.length) usage('missing --out value');
      opts.out = argv[i];
    } else if (arg === '--help' || arg === '-h') usage();
    else if (arg.startsWith('-')) usage('unknown option ' + arg);
    else if (opts.run) usage('unexpected argument ' + arg);
    else opts.run = arg;
  }
  if (!opts.run) usage('missing <run-dir>');
  if (opts.execute && opts.close) usage('--execute and --close are mutually exclusive');
  return opts;
}

// ---------------------------------------------------------------------------
// Dry plan. Read-only: only disk artifacts, no shared/rpc.cjs require.
// ---------------------------------------------------------------------------

function buildPlan(info) {
  const {run, session, module, moduleFile, connection, sdk, inspection} = info;
  const problems = [];

  const vi = display.resolveViSymbols(module);
  problems.push(...vi.problems);

  let surface = null;
  try { surface = display.requireExport(module, VI_SURFACE, 'vkCreateViSurfaceNN'); }
  catch (error) { problems.push(error.message); }

  // SDK functions are offsets into the session's sdk base, not module.base.
  const sdkBase = connection && connection.addresses && connection.addresses.sdk;
  const sdkPlan = {};
  for (const name of display.SDK_NAMES) {
    const entry = sdk.entries[name];
    sdkPlan[name] = {
      offset: entry.offset,
      offsetHex: '0x' + entry.offset.toString(16),
      address: Number.isSafeInteger(sdkBase) ? sdkBase + entry.offset : null,
      addressHex: Number.isSafeInteger(sdkBase) ? '0x' + (sdkBase + entry.offset).toString(16) : null,
      prefix: entry.prefix,
    };
    if (!Number.isSafeInteger(sdkBase)) problems.push('connection.json has no safe sdk base address');
  }
  if (sdkPlan.allocate.offset === sdkPlan.free.offset || sdkPlan.allocate.offset === sdkPlan.memset.offset ||
      sdkPlan.free.offset === sdkPlan.memset.offset) {
    problems.push('sdk-function offsets are not distinct');
  }

  const scratch = connection && connection.addresses && connection.addresses.scratch;
  if (!Number.isSafeInteger(scratch)) problems.push('connection.json has no safe scratch address');
  const p = Number.isSafeInteger(scratch) ? scratch + 0x10000 : null;

  // Cross-check only the NN vi entries this path actually consumes. The app's
  // unresolved DestroyLayer/CloseDisplay import slots are irrelevant because
  // this tool calls module exports directly (and only closes via --close).
  const crossInfos = new Map();
  for (const entry of [...Object.values(vi.symbols), vi.optional.initialize,
    vi.optional.getNativeWindowFree]) {
    if (entry && entry.exported) crossInfos.set(entry.name, entry);
  }
  const cross = display.crossCheckInspection(inspection, module, [...crossInfos.values()]);

  const symbolRef = (info) => info && info.exported ?
    {name: info.name, offset: info.offset, offsetHex: '0x' + info.offset.toString(16),
      address: info.address, addressHex: info.addressHex, size: info.size} : null;

  const steps = [
    {
      n: 1, phase: 'allocate', call: 'sdk.allocate',
      address: sdkPlan.allocate.address, addressHex: sdkPlan.allocate.addressHex,
      args: ['out=scratch+0x10000', 'alignment=' + LAYER_ALIGNMENT, 'size=0x' + LAYER_SIZE.toString(16)],
      expect: 'result == 0; u64 at scratch+0x10000 is the layer block (4 KiB aligned)',
      citation: 'vulkan-copy.cjs:44-45',
    },
    {
      n: 2, phase: 'allocate', call: 'sdk.memset',
      address: sdkPlan.memset.address, addressHex: sdkPlan.memset.addressHex,
      args: ['layer', '0', '0x' + LAYER_SIZE.toString(16)],
      expect: 'block is zero before construction',
      citation: 'vulkan-copy.cjs:45',
    },
    {
      n: 3, phase: 'display', call: 'nn::vi::OpenDefaultDisplay',
      address: symbolRef(vi.symbols.openDisplay),
      args: ['out=scratch+0x10008'],
      expect: 'status == 0; u64 at scratch+0x10008 is nn::vi::Display*',
      citation: 'vulkan-copy.cjs:46',
    },
    {
      n: 4, phase: 'layer', call: 'nn::vi::Layer::Layer()',
      address: symbolRef(vi.symbols.layerCtor),
      args: ['this=layer'],
      expect: 'void (constructor); do not test result',
      citation: 'vulkan-copy.cjs:47',
    },
    {
      n: 5, phase: 'layer', call: 'nn::vi::LayerCreationSettings(1920, 1080)',
      address: symbolRef(vi.symbols.layerSettingsCtor),
      args: ['this=layer+0x' + LAYER_SETTINGS_OFFSET.toString(16), WIDTH, HEIGHT],
      expect: 'void (constructor); settings live inside the 0x2000 block',
      citation: 'vulkan-copy.cjs:47',
    },
    {
      n: 6, phase: 'layer', call: 'write layer name',
      address: null,
      args: ['layer+0x' + LAYER_NAME_OFFSET.toString(16), Buffer.from(LAYER_NAME + '\0').toString('hex')],
      expect: 'NUL-terminated "' + LAYER_NAME + '" used by Layer::Open',
      citation: 'vulkan-copy.cjs:48',
    },
    {
      n: 7, phase: 'layer', call: 'nn::vi::Layer::Open',
      address: symbolRef(vi.symbols.layerOpen),
      args: ['layer', 'settings=layer+0x' + LAYER_SETTINGS_OFFSET.toString(16),
        'display', 'name=layer+0x' + LAYER_NAME_OFFSET.toString(16)],
      expect: 'status == 0; layer enters open state at layer+0x500',
      citation: 'vulkan-copy.cjs:49',
    },
    {
      n: 8, phase: 'validate', call: 'read layer+0x' + LAYER_STATE_OFFSET.toString(16),
      address: null,
      args: ['layer+0x' + LAYER_STATE_OFFSET.toString(16), 2],
      expect: 'byte0 open bit == 1; byte1 == 1 in the reference path',
      citation: 'independent-layer.cjs:34-36',
    },
    {
      n: 9, phase: 'window', call: 'nn::vi::Layer::GetNativeWindow()',
      address: symbolRef(vi.symbols.getNativeWindow),
      args: ['this=layer'],
      expect: 'u64 native window in x0 (member form); if only the free form is exported, ' +
        'call ' + VI.getNativeWindowFree + '(out=scratch+0x10010, layer)',
      citation: 'vulkan-copy.cjs:62; independent-layer.cjs:37',
    },
    {
      n: 10, phase: 'expose', call: 'resolve ' + VI_SURFACE,
      address: surface,
      args: ['module.base + export offset'],
      expect: 'absolute address emitted in vi-layer.json for the app context',
      citation: 'vulkan-copy.cjs:63; vi_present.c:14',
    },
    {
      n: 11, phase: 'retain', call: 'restore scratch only',
      address: null,
      args: [],
      expect: 'layer and display are retained; no Layer::Close, CloseDisplay, free, or Vulkan object',
      citation: 'vulkan-copy.cjs:101-104,204-223',
    },
  ];

  const closeSteps = [
    {
      n: 1, phase: 'safety', call: 'read ' + LAYER_FILE + '; verify session/created/layer',
      address: null,
      args: ['run=' + run, 'session=' + session],
      expect: 'recorded session == live session; layer > 0 and 4 KiB aligned',
      citation: 'requirement: session must match the current run',
    },
    {
      n: 2, phase: 'safety', call: 'refuse if another run artifact retains this layer',
      address: null,
      args: ['vulkan-display.json', 'vk-bringup.json', 'nes-surface.json'],
      expect: 'no retained surface/swapchain referencing layer (bypass with --force)',
      citation: 'vulkan-copy.cjs:206-219 (surface destroyed before layer)',
    },
    {
      n: 3, phase: 'safety', call: 'nn::vi::Layer::IsCloseable()',
      address: symbolRef(vi.optional.layerCloseable),
      args: ['this=layer'],
      expect: 'nonzero; this check is never bypassed by --force',
      citation: 'module export _ZNK2nn2vi5Layer11IsCloseableEv',
    },
    {
      n: 4, phase: 'close', call: 'nn::vi::Layer::Close(bool)',
      address: symbolRef(vi.optional.layerClose),
      args: ['layer', '0'],
      expect: 'void; resources released',
      citation: 'vulkan-copy.cjs:217; independent-layer.cjs:44',
    },
    {
      n: 5, phase: 'validate', call: 'read layer+0x' + LAYER_STATE_OFFSET.toString(16),
      address: null,
      args: ['layer+0x' + LAYER_STATE_OFFSET.toString(16), 2],
      expect: '== 00 00; otherwise do not free',
      citation: 'vulkan-copy.cjs:217',
    },
    {
      n: 6, phase: 'free', call: 'sdk.free',
      address: sdkPlan.free.address, addressHex: sdkPlan.free.addressHex,
      args: ['layer'],
      expect: 'allocation released after a clean Close',
      citation: 'vulkan-copy.cjs:219',
    },
  ];

  const warnings = [
    'dry mode is offline: it reads compatibility-module.json, connection.json, ' +
      'shared/sdk-functions.json and display-inspection.json only.',
    'execute requires a live run; connection.json session must equal the run directory ' +
      '(and shared/rpc.cjs addresses.session must agree).',
    'nn::vi::Initialize is assumed to have been called by the application; the reference ' +
      'display path does not call it.',
    'layer and display are intentionally retained; execute never calls Layer::Close, ' +
      'CloseDisplay, sdk.free, or any Vulkan entrypoint.',
    'window/layer handles are session-scoped; do not reuse vi-layer.json after the run stops.',
    'vkCreateViSurfaceNN is resolved from the module export table only, as required; ' +
      'no other Vulkan function is resolved or called.',
    'close mode never bypasses Layer::IsCloseable; --force only bypasses the retained-artifact check.',
  ];
  if (vi.symbols.getNativeWindow.name === VI.getNativeWindowFree) {
    warnings.push('member GetNativeWindow is absent; execute uses the free form with an out pointer.');
  }
  if (!vi.optional.closeDisplay.exported) {
    warnings.push('CloseDisplay is not exported; --close-display will be unavailable.');
  }

  return {
    format: PLAN_FORMAT,
    mode: 'dry',
    offline: true,
    run,
    session,
    module: {file: moduleFile, base: module.base, baseHex: '0x' + module.base.toString(16),
      symbolCount: module.symbols.length},
    scratch: {base: scratch, baseHex: Number.isSafeInteger(scratch) ? '0x' + scratch.toString(16) : null,
      planPointer: p, planPointerHex: Number.isSafeInteger(p) ? '0x' + p.toString(16) : null},
    sdk: sdkPlan,
    vi: {
      symbols: Object.fromEntries(Object.entries(vi.symbols).map(([k, v]) => [k, symbolRef(v)])),
      optional: Object.fromEntries(Object.entries(vi.optional).map(([k, v]) => [k, symbolRef(v)])),
    },
    vkCreateViSurfaceNN: symbolRef(surface),
    structure: {
      layerSize: LAYER_SIZE,
      layerAlignment: LAYER_ALIGNMENT,
      settingsOffset: LAYER_SETTINGS_OFFSET,
      nameOffset: LAYER_NAME_OFFSET,
      stateOffset: LAYER_STATE_OFFSET,
      width: WIDTH,
      height: HEIGHT,
      name: LAYER_NAME,
    },
    steps,
    closeSteps,
    crossChecks: cross.checks,
    warnings: [...warnings, ...cross.warnings],
    problems,
    output: {
      file: path.join(run, LAYER_FILE),
      requiredShape: {
        session: 'string (connection.json addresses.session)',
        window: 'number (u64 native window from Layer::GetNativeWindow)',
        layer: 'number (u64 nn::vi::Layer*)',
        display: 'number (u64 nn::vi::Display*)',
        layerAllocation: 'number (u64 sdk.allocate base; same block as layer)',
        width: WIDTH,
        height: HEIGHT,
        created: 'boolean true when the layer is open and retained',
        vkCreateViSurfaceNN: 'number (absolute module address for the app context)',
      },
    },
  };
}

function planDry(opts) {
  const info = display.loadRun(opts.run);
  return buildPlan(info);
}

// ---------------------------------------------------------------------------
// Live paths. Only reached with --execute/--close.
// ---------------------------------------------------------------------------

function loadRpc(info) {
  const rpc = require(path.join(info.run, 'shared/rpc.cjs'));
  const live = rpc.a && rpc.a.session;
  if (live !== info.session) {
    throw Error('session mismatch: live helper "' + live + '" != run session "' + info.session + '"');
  }
  return rpc;
}

function makeResolver(rpc, module) {
  const cache = {};
  return async function resolve(name) {
    if (cache[name]) return cache[name];
    const info = display.requireExport(module, name);
    const bytes = await rpc.read(info.address, Math.min(Math.max(info.size, 4), 32));
    await rpc.request({command: 'verify-function', address: info.address,
      hex: bytes.toString('hex')});
    return (cache[name] = info.address);
  };
}

async function verifySdk(rpc, sdk) {
  for (const name of display.SDK_NAMES) {
    const entry = sdk.entries[name];
    const actual = await rpc.read(rpc.a.sdk + entry.offset, entry.bytes.length / 2);
    if (actual.toString('hex') !== entry.bytes.toLowerCase()) {
      throw Error('SDK function "' + name + '" differs from shared/sdk-functions.json; ' +
        'refusing to call it');
    }
  }
}

async function execute(opts) {
  const info = display.loadRun(opts.run);
  const {run, session, module, sdk} = info;

  const layerFile = path.join(run, opts.file || LAYER_FILE);
  if (display.isFile(layerFile)) {
    let existing = null;
    try { existing = display.readJson(layerFile, LAYER_FILE); } catch { /* malformed: replace */ }
    if (existing && existing.created === true && !opts.force) {
      throw Error(LAYER_FILE + ' already records a retained layer (session ' +
        existing.session + ', layer ' + existing.layer + '); run --close first or pass --force');
    }
  }

  const vi = display.resolveViSymbols(module);
  if (vi.problems.length) throw Error(vi.problems.join('; '));
  const surface = display.requireExport(module, VI_SURFACE, 'vkCreateViSurfaceNN');

  const rpc = loadRpc(info);
  await verifySdk(rpc, sdk);
  const resolve = makeResolver(rpc, module);

  const address = {};
  for (const key of ['openDisplay', 'layerCtor', 'layerSettingsCtor', 'layerOpen', 'getNativeWindow']) {
    address[key] = await resolve(vi.symbols[key].name);
  }
  address.layerClose = vi.optional.layerClose.exported ? await resolve(VI.layerClose) : null;
  address.layerCloseable = vi.optional.layerCloseable.exported ? await resolve(VI.layerCloseable) : null;
  address.surface = surface.address;

  const callRaw = async (name, args) => rpc.callAddress(address[name], args);
  const callStatus = async (name, args) => {
    const reply = await callRaw(name, args);
    return Number(BigInt.asIntN(32, reply.result));
  };

  const p = rpc.a.scratch + 0x10000;
  const saved = await rpc.read(p, 0x10000);
  const pointer = async offset => {
    const value = (await rpc.read(p + offset, 8)).readBigUInt64LE();
    if (value === 0n) throw Error('null output handle at scratch+0x10000+0x' + offset.toString(16));
    if (value > BigInt(Number.MAX_SAFE_INTEGER)) throw Error('handle does not fit in a JSON number');
    return Number(value);
  };

  let layer = 0;
  let displayHandle = 0;
  try {
    await rpc.write(p, Buffer.alloc(0x10000));

    const allocated = await rpc.call(sdk.entries.allocate.offset,
      [p, LAYER_ALIGNMENT, LAYER_SIZE]);
    if (BigInt(allocated.result) !== 0n) throw Error('sdk.allocate failed');
    layer = await pointer(0);
    if (layer % LAYER_ALIGNMENT !== 0) throw Error('layer allocation is not 4 KiB aligned');

    // memset returns the destination pointer (see the d5CRTW operations log),
    // so its result is not a status.
    await rpc.call(sdk.entries.memset.offset, [layer, 0, LAYER_SIZE]);

    if (await callStatus('openDisplay', [p + 8]) !== 0) throw Error('OpenDefaultDisplay failed');
    displayHandle = await pointer(8);

    await callRaw('layerCtor', [layer]);
    await callRaw('layerSettingsCtor', [layer + LAYER_SETTINGS_OFFSET, WIDTH, HEIGHT]);
    await rpc.write(layer + LAYER_NAME_OFFSET, Buffer.from(LAYER_NAME + '\0'));
    if (await callStatus('layerOpen', [layer, layer + LAYER_SETTINGS_OFFSET, displayHandle,
      layer + LAYER_NAME_OFFSET]) !== 0) {
      throw Error('Layer::Open failed; allocation retained at 0x' + layer.toString(16));
    }

    const state = await rpc.read(layer + LAYER_STATE_OFFSET, 2);
    if (!(state[0] & 1)) throw Error('Layer did not enter the open state (layer+0x500=' +
      state.toString('hex') + '); allocation retained');
    const stateWarning = state[1] === 1 ? null :
      'layer+0x500 second byte is ' + state[1] + ', reference path expects 1';

    let window;
    if (vi.symbols.getNativeWindow.name === VI.getNativeWindow) {
      const reply = await callRaw('getNativeWindow', [layer]);
      window = BigInt.asUintN(64, reply.result);
    } else {
      // Free form: nn::vi::GetNativeWindow(void **out, Layer *layer)
      await callRaw('getNativeWindow', [p + 0x10, layer]);
      window = (await rpc.read(p + 0x10, 8)).readBigUInt64LE();
    }
    if (window === 0n || window > BigInt(Number.MAX_SAFE_INTEGER)) {
      throw Error('Layer::GetNativeWindow returned an unusable window handle: 0x' +
        window.toString(16));
    }

    const report = {
      format: FORMAT,
      session,
      window: Number(window),
      windowHex: '0x' + window.toString(16),
      layer,
      layerHex: '0x' + layer.toString(16),
      display: displayHandle,
      displayHex: '0x' + displayHandle.toString(16),
      layerAllocation: layer,
      layerAllocationSize: LAYER_SIZE,
      layerAllocationAlignment: LAYER_ALIGNMENT,
      width: WIDTH,
      height: HEIGHT,
      created: true,
      createdAt: new Date().toISOString(),
      run,
      vkCreateViSurfaceNN: surface.address,
      vkCreateViSurfaceNNHex: surface.addressHex,
      module: {base: module.base, baseHex: '0x' + module.base.toString(16)},
      retained: true,
      layerState: state.toString('hex'),
      functions: address,
      scratchRestored: false,
    };
    if (stateWarning) report.warnings = [stateWarning];

    // Scratch is not part of the retained object graph; restore it before the
    // record is written so the reported value is accurate. The layer itself is
    // never closed here.
    try {
      await rpc.write(p, saved);
      report.scratchRestored = true;
    } catch (error) {
      report.scratchRestoreError = error.message;
    }

    await writeJson(layerFile, report);
    console.error('vi-layer: layer 0x' + layer.toString(16) + ' display 0x' +
      displayHandle.toString(16) + ' window 0x' + window.toString(16) +
      ' (1920x1080) retained; ' + layerFile);
    return report;
  } finally {
    try {
      await rpc.write(p, saved);
    } catch (error) {
      console.error('vi-layer: scratch restore failed: ' + error.message);
    }
  }
}

async function closeLayer(opts) {
  const info = display.loadRun(opts.run);
  const {run, session, module, sdk} = info;
  const layerFile = path.join(run, opts.file || LAYER_FILE);
  const closeFile = path.join(run, opts.file ? opts.file.replace(/\.json$/, '-close.json') : CLOSE_FILE);

  if (!display.isFile(layerFile)) throw Error('missing ' + layerFile + '; nothing retained to close');
  const record = display.readJson(layerFile, LAYER_FILE);
  if (record.session !== session) {
    throw Error('session mismatch: ' + LAYER_FILE + ' session "' + record.session +
      '" != run session "' + session + '"');
  }
  if (record.created !== true) throw Error(LAYER_FILE + ' does not record a created layer');
  const layer = record.layer;
  if (!Number.isSafeInteger(layer) || layer <= 0 || layer % LAYER_ALIGNMENT !== 0) {
    throw Error('recorded layer is not a valid 4 KiB-aligned address: ' + layer);
  }
  const displayHandle = record.display;
  if (!Number.isSafeInteger(displayHandle) || displayHandle <= 0) {
    throw Error('recorded display is not a valid address: ' + displayHandle);
  }

  // A previous successful close is never replayed, even with --force: calling
  // Layer::Close or sdk.free on the freed block would be a use-after-free.
  if (display.isFile(closeFile)) {
    let previous = null;
    try { previous = display.readJson(closeFile, CLOSE_FILE); } catch { /* ignore */ }
    if (previous && previous.closed === true) {
      throw Error(closeFile + ' already records a closed layer; refusing to close twice');
    }
  }

  // Only if safe: another retained artifact that references this layer means a
  // Vulkan surface/swapchain may still be attached to the window.
  const artifacts = ['vulkan-display.json', 'vk-bringup.json', 'nes-surface.json'];
  for (const name of artifacts) {
    const file = path.join(run, name);
    if (!display.isFile(file)) continue;
    let data = null;
    try { data = display.readJson(file, name); } catch { continue; }
    const references = [];
    if (data.displayResources && data.displayResources.layer !== undefined) {
      references.push(data.displayResources.layer);
    }
    if (data.handles && data.handles.layer !== undefined) references.push(data.handles.layer);
    if (data.layer !== undefined) references.push(data.layer);
    if (references.some(value => Number(value) === layer) && !opts.force) {
      throw Error('refusing to close: ' + name + ' records retained resources for layer 0x' +
        layer.toString(16) + '; destroy the surface/swapchain first or pass --force');
    }
  }

  const vi = display.resolveViSymbols(module);
  if (vi.problems.length) throw Error(vi.problems.join('; '));
  if (!vi.optional.layerClose.exported) throw Error('module does not export Layer::Close');
  if (!vi.optional.layerCloseable.exported) throw Error('module does not export Layer::IsCloseable');

  const rpc = loadRpc(info);
  await verifySdk(rpc, sdk);
  const resolve = makeResolver(rpc, module);
  const closeAddress = await resolve(VI.layerClose);
  const closeableAddress = await resolve(VI.layerCloseable);
  const freeAddress = sdk.entries.free.offset;

  const callStatus = async (address, args) =>
    Number(BigInt.asIntN(32, (await rpc.callAddress(address, args)).result));

  const closeable = await callStatus(closeableAddress, [layer]);
  if (closeable === 0) {
    throw Error('Layer::IsCloseable returned false for layer 0x' + layer.toString(16) +
      '; refusing to close (this check is not bypassed by --force)');
  }

  // Layer::Close(bool) returns void, so the result is not interpreted.
  await rpc.callAddress(closeAddress, [layer, 0]);
  const state = await rpc.read(layer + LAYER_STATE_OFFSET, 2);
  if (!state.equals(Buffer.alloc(2))) {
    throw Error('layer+0x500 is ' + state.toString('hex') + ' after Layer::Close; ' +
      'resources are still retained, not freeing the allocation');
  }

  const errors = [];
  let freed = true;
  try {
    await rpc.call(freeAddress, [layer]);
  } catch (error) {
    freed = false;
    errors.push({action: 'sdk.free', error: error.message});
  }

  let closedDisplay = false;
  if (opts.closeDisplay) {
    try {
      if (!vi.optional.closeDisplay.exported) throw Error('module does not export CloseDisplay');
      const closeDisplayAddress = await resolve(VI.closeDisplay);
      await rpc.callAddress(closeDisplayAddress, [displayHandle]);
      closedDisplay = true;
    } catch (error) {
      errors.push({action: 'CloseDisplay', error: error.message});
    }
  }

  const report = {
    format: CLOSE_FORMAT,
    session,
    layer,
    layerHex: '0x' + layer.toString(16),
    display: displayHandle,
    displayHex: '0x' + displayHandle.toString(16),
    closed: true,
    freed,
    closedDisplay,
    layerState: state.toString('hex'),
    completed: new Date().toISOString(),
  };
  if (errors.length) report.errors = errors;
  await writeJson(closeFile, report);

  // Mark the layer record inactive so neither this tool nor the app treats the
  // freed handle as live. Only do this when the allocation is actually gone.
  if (freed) {
    await writeJson(layerFile, {...record, created: false, closed: true,
      closedAt: report.completed});
  }

  console.error('vi-layer: closed layer 0x' + layer.toString(16) +
    (freed ? ' and freed its allocation' : '; sdk.free FAILED, allocation still live') +
    (closedDisplay ? '; closed display 0x' + displayHandle.toString(16) : '; display retained') +
    '; ' + closeFile);
  if (!freed) throw Error('Layer::Close succeeded but sdk.free failed; see ' + closeFile);
  return report;
}

function writeJson(file, value) {
  const text = JSON.stringify(value, null, 2) + '\n';
  fs.writeFileSync(file, text, {mode: 0o600});
}

// ---------------------------------------------------------------------------

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  if (opts.close) {
    try { await closeLayer(opts); }
    catch (error) { console.error('vi-layer: ' + (error.stack || error.message)); process.exitCode = 1; }
    return;
  }
  if (opts.execute) {
    try { await execute(opts); }
    catch (error) { console.error('vi-layer: ' + (error.stack || error.message)); process.exitCode = 1; }
    return;
  }

  let plan;
  try { plan = planDry(opts); }
  catch (error) { console.error('vi-layer: ' + (error.stack || error.message)); process.exit(2); }
  const text = JSON.stringify(plan, null, 2) + '\n';
  if (opts.out) fs.writeFileSync(opts.out, text);
  process.stdout.write(text);
  if (!opts.quiet) {
    console.error('vi-layer (dry): ' + plan.run + ' session ' + plan.session);
    console.error('  layer ' + plan.structure.width + 'x' + plan.structure.height +
      ' order: allocate -> memset -> OpenDefaultDisplay -> LayerCtor -> SettingsCtor -> Open -> GetNativeWindow');
    console.error('  ' + VI_SURFACE + ' ' + (plan.vkCreateViSurfaceNN ?
      plan.vkCreateViSurfaceNN.addressHex : 'MISSING'));
    console.error('  cross-checks ' + plan.crossChecks.filter(c => c.ok).length + '/' +
      plan.crossChecks.length + ' ok; ' + plan.problems.length + ' problem(s)');
    if (plan.problems.length) for (const problem of plan.problems) console.error('  problem: ' + problem);
  }
  if (plan.problems.length) process.exitCode = 1;
}

if (require.main === module) {
  main().catch(error => {
    console.error('vi-layer: ' + (error.stack || error.message));
    process.exit(1);
  });
}

module.exports = {buildPlan, execute, closeLayer, FORMAT, PLAN_FORMAT, CLOSE_FORMAT};
