'use strict';
/*
 * runtime.cjs - the launcher/app lifecycle.
 *
 * Flow (see docs/ARCHITECTURE.md):
 *
 *   map launcher code RX at 0x08000000, data RW at 0x0A000000
 *   -> call the launcher entry; it runs a frame budget or returns a launch
 *      request through the shared control page
 *   -> map the app package (code RX, data RW, heap, stack, assets)
 *   -> call the app entry; it returns when the user exits
 *   -> unmap the app, free its blocks, call the launcher again with
 *      HBX_FLAG_RESUME so it can show "returned from ..."
 *
 * The launcher image and the Vulkan display stay mapped and alive the whole
 * time; only app images are unmapped.
 */
const fs = require('node:fs');
const path = require('node:path');
const {HostError, check, hex, readJson, sha256, sleep} = require('./common.cjs');
const {scan, buildCatalog} = require('./packages.cjs');
const {buildDispatch} = require('./dispatch.cjs');
const {PAGE} = require('./target.cjs');

const ROOT = path.resolve(__dirname, '..', '..');
const ABI = readJson(path.join(ROOT, 'sdk/abi.json'));

const LAUNCHER_CODE_BASE = 0x08000000;
const LAUNCHER_DATA_BASE = 0x0A000000;

const FLAG_RESUME = 0x1;
const FLAG_NO_AV = 0x4;
const EXIT = 0xffffffff;

const pageUp = value => Math.ceil(Number(value) / PAGE) * PAGE;

class Ctx {
  constructor() {
    this.buffer = Buffer.alloc(ABI.ctxSize);
  }
  u32(field, value) { this.buffer.writeUInt32LE(value >>> 0, ABI.ctx[field]); return this; }
  u64(field, value) { this.buffer.writeBigUInt64LE(BigInt(value), ABI.ctx[field]); return this; }
  g32(field, value) { this.buffer.writeUInt32LE(value >>> 0, ABI.ctx.gpu + ABI.gpu[field]); return this; }
  g64(field, value) { this.buffer.writeBigUInt64LE(BigInt(value), ABI.ctx.gpu + ABI.gpu[field]); return this; }
  get32(field) { return this.buffer.readUInt32LE(ABI.ctx[field]); }
  get64(field) { return this.buffer.readBigUInt64LE(ABI.ctx[field]); }
  static header() {
    return new Ctx().u32('magic', ABI.magic).u32('abi_version', ABI.abiVersion)
      .u32('size', ABI.ctxSize);
  }
}

class Runtime {
  constructor(options) {
    this.target = options.target;
    this.runDir = options.runDir || null;
    this.appsDir = options.appsDir || path.join(ROOT, 'apps');
    this.log = options.log;
    this.options = options;
    this.packages = [];
    this.catalog = null;
    this.blocks = [];
    this.started = Date.now();
    this.launcher = null;
    this.display = null;
    this.dispatch = null;
    this.shared = {gpu: null, ui: 0};
    this.input = {};
    this.mappedImages = [];
    this.tickHz = 0;
    this.tickOffset = 1218112;   /* SDK GetSystemTick helper: svc #0x1e; ret */
  }

  elapsedNs() {
    return BigInt(Math.round((Date.now() - this.started) * 1e6));
  }

  async open() {
    const live = this.target.kind === 'live';
    this.log.stage('open', {backend: this.target.kind, run: this.runDir});

    /* Packages and the catalog the launcher reads. */
    const scanned = scan(this.appsDir, this.log);
    this.packages = scanned.packages;
    this.packageErrors = scanned.errors;
    this.catalog = buildCatalog(this.packages);
    this.log.info(`catalog: ${this.packages.length} title(s), ${this.catalog.iconLayers} icon(s)`,
      {rejected: scanned.errors.length});

    /* Display: a retained NN vi layer + window. */
    this.display = await this.createDisplay(live);

    /* Vulkan dispatch table for the current process. */
    if (live) {
      const moduleFile = path.join(this.runDir, 'compatibility-module.json');
      check(fs.existsSync(moduleFile), 'compatibility-module.json missing from the run directory');
      const module = readJson(moduleFile);
      this.dispatch = buildDispatch(module, {header: path.join(ROOT, 'sdk/include/hbx_vk.h')});
      this.log.info('dispatch table built', this.dispatch.manifest);
    } else {
      const {parseDispatchHeader} = require('./dispatch.cjs');
      const fields = parseDispatchHeader(path.join(ROOT, 'sdk/include/hbx_vk.h'));
      this.dispatch = {buffer: Buffer.alloc(fields.length * 8),
        manifest: {fields: fields.length, bytes: fields.length * 8, base: 'mock'}};
    }

    await this.resolveClock();
    await this.mapLauncher();
    await this.allocateShared();
    return this;
  }

  /* Measure the system tick rate by sampling the SDK's GetSystemTick helper
   * across a known wall-clock interval.  MRS CNTVCT traps at EL0 here, so the
   * rate cannot be read from CNTFRQ_EL0. */
  async resolveClock() {
    if (this.target.kind !== 'live') { this.tickHz = 0; return; }
    const address = Number(this.target.rpc.a.sdk) + this.tickOffset;
    try {
      const bytes = await this.target.rpc.read(address, 8);
      await this.target.rpc.request({command: 'verify-function', address,
        hex: bytes.toString('hex')});
      const first = BigInt((await this.target.rpc.request({command: 'call-address',
        address, args: []})).result);
      await sleep(400);
      const second = BigInt((await this.target.rpc.request({command: 'call-address',
        address, args: []})).result);
      const delta = Number(second - first);
      if (delta > 0) this.tickHz = Math.round(delta / 0.4 / 1000) * 1000;
      this.log.info('system tick measured', {hz: this.tickHz || 'fallback'});
    } catch (error) {
      this.log.warn('tick helper unavailable: ' + error.message);
    }
    if (!this.tickHz) this.tickHz = 19200000;
  }

  async createDisplay(live) {
    if (!live) {
      return {window: 0x11110000, vkCreateViSurfaceNN: 0x22220000, session: 'mock'};
    }
    const displayTools = require('./vk-display.cjs');
    const viLayer = require('./vi-layer.cjs');
    require('./inspect.cjs').ensureModuleArtifacts(this.runDir, this.log);
    const info = displayTools.loadRun(this.runDir);
    const file = path.join(this.runDir, 'vi-layer.json');
    const existing = (() => {
      try { return JSON.parse(fs.readFileSync(file, 'utf8')); } catch { return null; }
    })();
    if (existing && existing.created && String(existing.session) === String(info.session)) {
      this.log.info('reusing retained vi layer', {window: hex(existing.window)});
      this.launcherLayer = existing;
      return existing;
    }
    this.log.stage('vi-layer', {session: info.session});
    const record = await viLayer.execute({run: this.runDir, force: !!existing});
    check(record && record.created, 'vi layer was not created');
    this.log.info('vi layer ready', {window: hex(record.window), layer: hex(record.layer)});
    this.launcherLayer = record;
    return record;
  }

  async mapImage(label, entry) {
    await this.target.mapImage(entry);
    this.mappedImages.push({label, ...entry});
  }

  /* Returns the source blocks that are still aliased (unmap failed): those
   * must never be freed, or the allocator hands out locked memory. */
  async releaseImages(labels) {
    const stuck = new Set();
    for (const entry of this.mappedImages.filter(item => labels.includes(item.label))) {
      try { await this.target.unmapImage(entry); }
      catch (error) {
        stuck.add(entry.source);
        this.log.warn('unmap failed; leaking its source block: ' + error.message);
      }
    }
    this.mappedImages = this.mappedImages.filter(item => !labels.includes(item.label));
    return stuck;
  }

  async alloc(label, size, align = PAGE) {
    const bytes = Math.max(PAGE, pageUp(size));
    const base = await this.target.allocate(bytes, align);
    this.blocks.push({label, base, bytes});
    this.log.info(`allocated ${label}`, {address: hex(base), bytes});
    return base;
  }

  async mapLauncher() {
    const started = Date.now();
    /* A snapshot directory lets a run proceed while launcher sources are being
     * edited/built; defaults to the live build output. */
    const buildDir = process.env.HBL_LAUNCHER_BUILD || path.join(ROOT, 'launcher/build');
    const report = readJson(path.join(buildDir, 'report.json'));
    const code = fs.readFileSync(path.join(buildDir, 'code.bin'));
    const data = fs.readFileSync(path.join(buildDir, 'data.bin'));
    check(report.codeSha256 === sha256(code), 'launcher code.bin does not match report.json');
    const dataBytes = Math.max(0, Number(BigInt(report.bssEnd) - BigInt(report.dataBase)));

    /* A previous hbl-start in the same SmileBASIC session leaves the launcher
       resident (the display is retained by design).  Detect it by its entry
       bytes and reuse it instead of mapping a second copy, which would fight
       over the same virtual range. */
    const resident = await this.target.query(LAUNCHER_CODE_BASE);
    if (Number(resident.type) !== 0) {
      let same = false;
      let dataPresent = false;
      try {
        const mapped = await this.target.read(LAUNCHER_CODE_BASE, code.length);
        same = sha256(mapped) === report.codeSha256;
        const dataRegion = await this.target.query(LAUNCHER_DATA_BASE);
        dataPresent = Number(dataRegion.type) !== 0;
      } catch { same = false; }
      if (same && !dataPresent) {
        throw new HostError(
          'a previous launcher image is resident without its data page; ' +
          'restart SmileBASIC and re-acquire 4/4', 'resident-broken');
      }
      if (same) {
        this.launcher = {
          report, code, data, codeSource: 0, dataSource: 0, dataBytes,
          flush: {calls: 0}, entry: Number(BigInt(report.entry)),
          codeBase: LAUNCHER_CODE_BASE, dataBase: LAUNCHER_DATA_BASE, resident: true
        };
        this.log.info('reusing the resident launcher image', {entry: hex(this.launcher.entry)});
        return;
      }
      throw new HostError(
        `${hex(LAUNCHER_CODE_BASE)} is occupied by something that is not this launcher; ` +
        'restart SmileBASIC and re-acquire 4/4', 'address-busy');
    }

    const codeSource = await this.alloc('launcher-code-source', code.length);
    await this.target.write(codeSource, code);
    await this.mapImage('launcher-code', {source: codeSource, destination: LAUNCHER_CODE_BASE,
      bytes: code.length, permission: 5});
    const flush = await this.target.flushCode(LAUNCHER_CODE_BASE, code);

    const dataSource = await this.alloc('launcher-data-source', dataBytes || PAGE);
    {
      const region = Buffer.alloc(dataBytes || PAGE);
      if (data.length) data.copy(region, 0, 0, Math.min(data.length, region.length));
      await this.target.write(dataSource, region);
    }
    await this.mapImage('launcher-data', {source: dataSource, destination: LAUNCHER_DATA_BASE,
      bytes: dataBytes || PAGE, permission: 3});

    this.launcher = {
      report, code, data, codeSource, dataSource, dataBytes, flush,
      entry: Number(BigInt(report.entry)), codeBase: LAUNCHER_CODE_BASE,
      dataBase: LAUNCHER_DATA_BASE
    };
    this.log.info('launcher mapped', {
      entry: hex(this.launcher.entry), pages: Math.ceil(code.length / PAGE),
      dataBytes, cacheLines: flush.calls, milliseconds: Date.now() - started
    });
  }

  async allocateShared() {
    this.control = await this.alloc('control', 0x1000);
    this.ctxBlock = await this.alloc('ctx', 0x1000);
    this.logRing = await this.alloc('log-ring', 0x8000);
    this.logStruct = await this.alloc('log-struct', PAGE);
    this.heap = await this.alloc('launcher-heap', 8 * 1024 * 1024);
    this.stack = await this.alloc('launcher-stack', 1024 * 1024);
    this.padState = await this.alloc('pad-state', 0x1000);
    this.touchState = await this.alloc('touch-state', 0x1000);
    this.dispatchBlock = await this.alloc('dispatch', this.dispatch.buffer.length);
    this.catalogCapacity = 16384;                 /* 64 titles + strings */
    this.iconCapacity = 16 * 96 * 96 * 4;      /* HBX_MAX_ICON_LAYERS */
    this.catalogBlock = await this.alloc('catalog', this.catalogCapacity);
    this.iconsBlock = await this.alloc('icons', this.iconCapacity);
    this.atlasBlock = await this.alloc('font-atlas', 1024 * 1024);
    this.logRingSize = 0x8000;

    await this.target.write(this.ctxBlock, Buffer.alloc(0x1000));
    await this.target.write(this.logRing, Buffer.alloc(this.logRingSize));
    {
      const descriptor = Buffer.alloc(16);
      descriptor.writeBigUInt64LE(BigInt(this.logRing), 0);
      descriptor.writeUInt32LE(this.logRingSize, 8);
      descriptor.writeUInt32LE(0, 12);
      await this.target.write(this.logStruct, descriptor);
    }
    await this.target.write(this.padState, Buffer.alloc(0x1000));
    await this.target.write(this.touchState, Buffer.alloc(0x1000));
    await this.target.write(this.dispatchBlock, this.dispatch.buffer);
    check(this.catalog.block.length <= this.catalogCapacity, 'catalog exceeds its block');
    await this.target.write(this.catalogBlock, this.catalog.block);
    await this.target.write(this.iconsBlock, this.catalog.icons);
    const atlas = fs.readFileSync(path.join(ROOT, 'assets/font_atlas.r8'));
    check(atlas.length === 1024 * 1024, 'font atlas must be 1024x1024 R8');
    await this.target.write(this.atlasBlock, atlas);

    const control = Buffer.alloc(0x1000);
    control.writeUInt32LE(0x4c544348, 0); /* HCTL */
    await this.target.write(this.control, control);

    await this.resolveInput();
  }

  async resolveInput() {
    if (this.target.kind !== 'live') {
      this.input = {pad_handheld: 0, pad_full_key: 0, pad_joy_dual: 0,
        pad_joy_left: 0, pad_joy_right: 0, touch: 0};
      return;
    }
    const sdk = Number(this.target.rpc.a.sdk);
    check(Number.isSafeInteger(sdk) && sdk > 0, 'connection.json has no SDK base');
    const offsets = {pad_handheld: 4845408, pad_full_key: 4845184,
      pad_joy_dual: 4847136, pad_joy_left: 4847360, pad_joy_right: 4847584};
    const names = {pad_handheld: 'padHandheld', pad_full_key: 'padFullKey',
      pad_joy_dual: 'padJoyDual', pad_joy_left: 'padJoyLeft',
      pad_joy_right: 'padJoyRight'};

    /* The archived SDK ships the known-good instruction bytes for this build.
     * Verify before calling: a wrong offset would execute garbage and take the
     * process down (this is exactly what run-cpu.cjs did). */
    const hidFile = process.env.HBL_HID_SYMBOLS || path.join(path.dirname(ROOT),
      'archive/hblauncher-cpu-sdk/sdk/native/hid-functions.json');
    let hid = null;
    try { hid = JSON.parse(fs.readFileSync(hidFile, 'utf8')); }
    catch (error) { this.log.warn('hid symbol file unavailable: ' + hidFile); }

    const verified = {};
    for (const [field, offset] of Object.entries(offsets)) {
      const address = sdk + offset;
      let ok = false;
      try {
        if (hid && hid[names[field]] && hid[names[field]].hex) {
          await this.target.rpc.request({command: 'verify-function', address,
            hex: hid[names[field]].hex});
          ok = true;
        } else {
          const bytes = await this.target.rpc.read(address, 4);
          ok = bytes.some(byte => byte !== 0);
        }
      } catch (error) {
        ok = false;
        this.log.warn(`pad source ${field} rejected: ${error.message}`);
      }
      verified[field] = ok ? address : 0;
      if (!ok) this.log.warn(`pad source ${field} disabled (failed verification)`);
    }
    this.input = verified;
    this.input.touch = await this.resolveTouch();
    this.audio = await this.resolveAudio();
    this.log.info('input sources resolved',
      {verified: Object.entries(verified).filter(([, v]) => v).length, touch: hex(this.input.touch)});
  }

  /* nn::audio AudioOut entrypoints, resolved from the module export table.
   * Offsets are the audited ones from the archived SDK's audio-symbols.json. */
  async resolveAudio() {
    const offsets = {
      initialize_parameter: 1867568,
      open_default_audio_out: 1969792,
      sample_rate: 1971728,
      channel_count: 1971744,
      sample_format: 1971760,
      set_buffer_info: 1867584,
      append_buffer: 1971776,
      start: 1971232,
      stop: 1971360,
      buffer_count: 1972336,
      flush_buffers: 1972672,
      released_buffer: 1971984,
      close: 1970944
    };
    if (this.target.kind !== 'live') return {};
    const sdk = Number(this.target.rpc.a.sdk);
    if (!Number.isSafeInteger(sdk) || sdk <= 0) return {};
    const resolved = {};
    let ok = 0;
    for (const [name, offset] of Object.entries(offsets)) {
      const address = sdk + offset;
      let accepted = false;
      for (let attempt = 0; attempt < 2 && !accepted; attempt++) {
        try {
          const region = await this.target.rpc.query(address);
          const executable = (Number(region.permissions) & 1) && (Number(region.permissions) & 4);
          const bytes = executable ? await this.target.rpc.read(address, 16) : null;
          accepted = !!bytes && bytes.some(byte => byte !== 0);
        } catch (error) {
          if (attempt === 1) this.log.warn(`audio function ${name} unavailable: ${error.message}`);
        }
      }
      if (accepted) { resolved[name] = address; ok++; }
    }
    /* All or nothing: a wrong entrypoint would execute garbage. */
    this.log.info('audio entrypoints resolved', {verified: ok, total: Object.keys(offsets).length});
    return ok === Object.keys(offsets).length ? resolved : {};
  }

  async resolveTouch() {
    const inspection = path.join(this.runDir, 'input-inspection.json');
    if (fs.existsSync(inspection)) {
      const record = readJson(inspection);
      const entry = record.imports &&
        record.imports['_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE'];
      if (entry && Number(entry.target) > 0) return Number(entry.target);
    }
    const imports = path.join(ROOT, 'host/data/input-imports.json');
    if (fs.existsSync(imports)) {
      const table = readJson(imports);
      const entry = table['_ZN2nn3hid19GetTouchScreenStateILm16EEEvPNS0_16TouchScreenStateIXT_EEE'];
      const main = Number(this.target.rpc.a.main);
      if (entry && Number.isSafeInteger(main)) {
        const bytes = await this.target.rpc.read(main + entry.slot, 8);
        const target = Number(bytes.readBigUInt64LE());
        if (target > 0 && target !== main + 0x29b850) {
          this.log.warn('touch entrypoint resolved from a raw import slot', {address: hex(target)});
          return target;
        }
      }
    }
    this.log.warn('touchscreen entrypoint not resolved; touch input will report no points');
    return 0;
  }

  /* --- context fill -------------------------------------------------- */

  fillLauncherCtx(options = {}) {
    const ctx = Ctx.header()
      .u32('flags', options.resume ? FLAG_RESUME : 0)
      .u64('status', 0).u64('exit', 0).u64('frame', 0)
      .u64('now_ns', this.elapsedNs()).u64('tick_hz', this.tickHz || 0).u64('uptime_ns', this.elapsedNs())
      .u64('heap_base', this.heap).u64('heap_size', 8 * 1024 * 1024)
      .u64('data_base', 0).u64('data_size', 0)
      .u64('code_base', this.launcher.codeBase)
      .u64('code_size', this.launcher.code.length)
      .u64('stack_base', this.stack).u64('stack_size', 1024 * 1024)
      .u64('control', this.control)
      .u64('catalog', this.catalogBlock)
      .u64('catalog_bytes', this.catalog.block.length)
      .u64('icons', this.iconsBlock || 0)
      .u64('icons_bytes', this.catalog.icons.length)
      .u64('assets', 0)
      .u64('log', this.logStruct)
      .u64('ui', 0)
      .u64('atlas', this.atlasBlock).u64('atlas_bytes', 1024 * 1024)
      .g64('dispatch', this.dispatchBlock)
      .g64('vi_window', this.display.window)
      .g64('create_vi_surface', this.display.vkCreateViSurfaceNN)
      .g64('destroy_surface', this.display.vkDestroySurfaceKHR || 0);
    /* GPU width/height are u32 in the ABI. */
    ctx.buffer.writeUInt32LE(1920, ABI.ctx.gpu + ABI.gpu.width);
    ctx.buffer.writeUInt32LE(1080, ABI.ctx.gpu + ABI.gpu.height);
    /* Inputs. */
    const inputBase = ABI.ctx.input;
    const padFields = ['pad_handheld', 'pad_full_key', 'pad_joy_dual',
      'pad_joy_left', 'pad_joy_right'];
    padFields.forEach((name, index) => {
      ctx.buffer.writeBigUInt64LE(BigInt(this.input[name] || 0), inputBase + index * 8);
    });
    ctx.buffer.writeBigUInt64LE(BigInt(this.input.touch || 0), inputBase + 5 * 8);
    ctx.buffer.writeBigUInt64LE(BigInt(this.padState), inputBase + 6 * 8);
    ctx.buffer.writeBigUInt64LE(BigInt(this.touchState), inputBase + 7 * 8);
    /* Arguments. */
    ctx.buffer.writeBigUInt64LE(BigInt(options.sequence || 0), ABI.ctx.args);
    ctx.buffer.writeBigUInt64LE(BigInt(options.resumeIndex || 0), ABI.ctx.args + 8);
    ctx.buffer.writeBigUInt64LE(BigInt(options.resumeStatus || 0), ABI.ctx.args + 16);
    ctx.buffer.writeBigUInt64LE(BigInt(options.maxFrames || 720), ABI.ctx.args + 24);
    this.writeAudio(ctx);
    /* args[6] is a diagnostic stage limit for the display bring-up (0 = full). */
    ctx.buffer.writeBigUInt64LE(BigInt(options.stage || 0), ABI.ctx.args + 48);
    return ctx;
  }

  async readControl() {
    const bytes = await this.target.read(this.control, 24);
    return {magic: bytes.readUInt32LE(0), launch: bytes.readUInt32LE(4),
      launchHash: bytes.readUInt32LE(8), lastStatus: bytes.readUInt32LE(12),
      frame: bytes.readUInt32LE(16), flags: bytes.readUInt32LE(20)};
  }

  async clearLaunch() {
    const zero = Buffer.alloc(4);
    await this.target.write(this.control + 4, zero);
  }

  async snapshotShared() {
    const bytes = await this.target.read(this.ctxBlock, ABI.ctxSize);
    this.shared.ui = Number(bytes.readBigUInt64LE(ABI.ctx.ui));
    this.shared.gpu = bytes.subarray(ABI.ctx.gpu, ABI.ctx.gpu + ABI.gpuSize);
    this.shared.launcherStatus = bytes.readBigUInt64LE(ABI.ctx.status);
    this.log.info('launcher shared state', {ui: hex(this.shared.ui)});
  }

  /* Y in the launcher sets control bit 1.  Rescan apps/ and rewrite the
   * catalog and icon blocks in place, then clear the request bit. */
  writeAudio(ctx) {
    const fields = ['initialize_parameter', 'open_default_audio_out', 'sample_rate',
      'channel_count', 'sample_format', 'set_buffer_info', 'append_buffer',
      'start', 'stop', 'buffer_count', 'flush_buffers', 'released_buffer', 'close'];
    fields.forEach((name, index) => {
      ctx.buffer.writeBigUInt64LE(BigInt((this.audio && this.audio[name]) || 0),
        ABI.ctx.audio + index * 8);
    });
  }

  async refreshCatalog() {
    const scanned = scan(this.appsDir, this.log);
    const catalog = buildCatalog(scanned.packages);
    if (catalog.block.length > this.catalogCapacity ||
        catalog.icons.length > this.iconCapacity) {
      this.log.error('refresh: catalog does not fit its block; keeping the old one');
      return;
    }
    this.packages = scanned.packages;
    this.catalog = catalog;
    await this.target.write(this.catalogBlock, catalog.block);
    await this.target.write(this.iconsBlock, catalog.icons);
    const flags = (await this.target.read(this.control + 20, 4)).readUInt32LE(0);
    const word = Buffer.alloc(4);
    word.writeUInt32LE(flags & ~2);
    await this.target.write(this.control + 20, word);
    this.log.stage('catalog refreshed', {titles: scanned.packages.length,
      icons: catalog.iconLayers, rejected: scanned.errors.length});
  }

  async readLog() {
    try {
      const bytes = await this.target.read(this.logRing, this.logRingSize);
      const text = bytes.toString('utf8').replace(/\0+$/, '');
      if (text.trim()) this.log.info('app log', {text: text.slice(-2000)});
    } catch { /* logging is best effort */ }
  }

  /* --- launcher loop ------------------------------------------------- */

  async run(order = {}) {
    let sequence = 0;
    let resume = null;
    let launches = 0;
    const maxEntries = order.maxEntries || 64;

    for (; sequence < maxEntries; sequence++) {
      if (order.deadline && Date.now() > order.deadline) {
        this.log.warn('time budget reached; stopping between entries');
        break;
      }
      const ctx = this.fillLauncherCtx({
        sequence,
        resume: !!resume,
        resumeIndex: resume ? resume.index : 0,
        resumeStatus: resume ? resume.status : 0,
        maxFrames: order.maxFrames || 720,
        stage: order.stage || 0
      });
      await this.target.write(this.ctxBlock, ctx.buffer);
      this.log.stage('launcher entry', {sequence, resume: !!resume});
      const reply = await this.callEntry(this.target, this.launcher.entry,
        this.launcher.code, [this.ctxBlock]);
      const status = Number((await this.target.read(this.ctxBlock + ABI.ctx.status, 8))
        .readBigUInt64LE());
      const control = await this.readControl();
      this.log.info('launcher returned', {status, launch: control.launch, reply: reply.result});
      /* The launcher's own ring carries the display stage trail. */
      await this.readLog();
      if (control.flags & 2) await this.refreshCatalog();
      /* The launcher publishes its renderer and GPU handles on its first
         entry; apps borrow them. */
      if (sequence === 0) await this.snapshotShared();

      if (status !== 0) {
        await this.readLog();
        throw new HostError('launcher entry failed with status ' + status, 'launcher');
      }
      if (control.launch === EXIT) {
        this.log.stage('exit requested');
        break;
      }
      if (control.launch >= 1) {
        /* Prefer the id hash: it survives catalog reordering between
           builds, unlike the 1-based index. */
        let index = control.launch - 1;
        if (control.launchHash) {
          const found = this.packages.findIndex(pkg => pkg.idHash === control.launchHash);
          if (found >= 0) index = found;
          else this.log.warn('launch hash does not match any package; using the index',
            {hash: control.launchHash});
        }
        const pkg = this.packages[index];
        if (!pkg) {
          this.log.warn('launcher requested an unknown title index', {index});
          await this.clearLaunch();
          resume = null;
          continue;
        }
        launches++;
        await this.clearLaunch();
        const appStatus = await this.launchPackage(pkg);
        await this.clearLaunch();
        resume = {index: index + 1, status: appStatus};
        this.log.stage('app returned', {id: pkg.manifest.id, status: appStatus});
        continue;
      }
      resume = null;
    }
    this.log.stage('session complete', {entries: sequence, launches});
    return {entries: sequence, launches};
  }

  /* --- app lifecycle ------------------------------------------------- */

  async launchPackage(pkg) {
    const kind = pkg.manifest.kind;
    this.log.stage('launch', {id: pkg.manifest.id, kind});
    if (kind === 'native') return this.launchNative(pkg);
    return require('./compat.cjs').launch(this, pkg);
  }

  buildAssetBlock(pkg) {
    check(pkg.assets.length <= 64, `${pkg.manifest.id}: at most 64 assets are supported`);
    /* struct hbx_asset_table: 16-byte header + 64 entries of 16 bytes. */
    const headerBytes = 16 + 64 * 16;
    let cursor = headerBytes;
    const names = [];
    const payload = [];
    for (const asset of pkg.assets) {
      const nameBytes = Buffer.from(asset.name + '\0', 'utf8');
      const nameOffset = cursor;
      names.push(Buffer.from(nameBytes));
      cursor += nameBytes.length;
      cursor = (cursor + 15) & ~15;
      const dataOffset = cursor;
      payload.push({nameOffset, dataOffset, bytes: asset.bytes});
      cursor += asset.bytes.length;
      cursor = (cursor + 15) & ~15;
    }
    const total = Math.max(PAGE, pageUp(cursor));
    const block = Buffer.alloc(total);
    block.writeUInt32LE(0x54415341, 0); /* ASAT */
    block.writeUInt32LE(pkg.assets.length, 4);
    /* Names and data interleave (name, data, name, data...); use the offsets
       laid out above.  Packing the names separately put the second name on
       top of the first asset's data. */
    pkg.assets.forEach((asset, index) => {
      const nameBytes = Buffer.from(asset.name + '\0', 'utf8');
      const nameAt = payload[index].nameOffset;
      nameBytes.copy(block, nameAt);
      const dataAt = payload[index].dataOffset;
      block.writeUInt32LE(nameAt, 16 + index * 16);
      block.writeUInt32LE(dataAt, 16 + index * 16 + 4);
      block.writeUInt32LE(asset.bytes.length, 16 + index * 16 + 8);
      block.writeUInt32LE(0, 16 + index * 16 + 12);
      payload[index].bytes.copy(block, dataAt);
    });
    return block;
  }

  /* Apps may leave progress words in ctx->args[2..5] (see apps/nes/src/app.c);
   * decode them so a slow or stuck run can be diagnosed after it returns. */
  async logProgress(id) {
    const args = await this.target.read(this.ctxBlock + ABI.ctx.args + 16, 32);
    const word = i => args.readBigUInt64LE(i * 8);
    const stage = word(1), audio = word(2), counts = word(3);
    if (!word(0) && !stage && !audio && !counts) return;
    const s8 = v => (Number(v & 0xffn) << 24) >> 24;
    this.log.info('app progress', {id,
      buttons: '0x' + word(0).toString(16),
      stage: Number(stage >> 32n), present: Number(BigInt.asIntN(32, stage & 0xffffffffn)),
      audio: {open: s8(audio), started: Number((audio >> 8n) & 0xffn),
        queued: Number((audio >> 16n) & 0xffn), error: s8(audio >> 24n),
        opened: Number((audio >> 32n) & 1n), deviceOpen: Number((audio >> 33n) & 1n),
        enabled: Number((audio >> 34n) & 1n),
        appended: Number(counts >> 32n), released: Number(counts & 0xffffffffn)}});
  }

  async callEntry(target, address, bytes, args) {
    if (target.verify) await target.verify(address, bytes);
    return target.call(address, args);
  }

  /* Diagnostic: ensure the display exists (one launcher entry), then launch a
   * named app once with ctx->args[1] = stage and return its status. */
  async launchOneApp(order) {
    const pkg = this.packages.find(item => item.manifest.id === order.appId);
    check(pkg, 'unknown app ' + order.appId);
    const ctx = this.fillLauncherCtx({sequence: 0, maxFrames: 1});
    await this.target.write(this.ctxBlock, ctx.buffer);
    await this.callEntry(this.target, this.launcher.entry, this.launcher.code,
      [this.ctxBlock]);
    await this.snapshotShared();
    const status = await this.launchNative(pkg, {stage: order.appStage || 0});
    /* Give the screen back: one launcher entry rebuilds a released display. */
    try {
      const after = this.fillLauncherCtx({sequence: 1, maxFrames: 30});
      await this.target.write(this.ctxBlock, after.buffer);
      await this.callEntry(this.target, this.launcher.entry, this.launcher.code, [this.ctxBlock]);
    } catch (error) {
      this.log.warn('launcher re-entry after the app failed: ' + error.message);
    }
    return status;
  }

  async launchNative(pkg, options = {}) {
    const report = pkg.report;
    const codeBase = Number(BigInt(report.codeBase));
    const dataBase = Number(BigInt(report.dataBase));
    const code = pkg.code;
    const data = pkg.data;
    const dataBytes = Math.max(PAGE, Number(report.dataRegionBytes || 0));

    /* Refuse to collide with the resident launcher or with itself. */
    const overlaps = (a, sizeA, b, sizeB) => a < b + sizeB && b < a + sizeA;
    const launcherCode = 0x100000;
    const launcherData = 0x100000;
    check(codeBase % PAGE === 0 && dataBase % PAGE === 0,
      `app ${pkg.manifest.id} bases must be page aligned`);
    for (const [label, base, size] of [['code', codeBase, pageUp(code.length)],
                                       ['data', dataBase, dataBytes]]) {
      check(!overlaps(base, size, LAUNCHER_CODE_BASE, launcherCode),
        `app ${pkg.manifest.id} ${label} overlaps the resident launcher code`);
      check(!overlaps(base, size, LAUNCHER_DATA_BASE, launcherData),
        `app ${pkg.manifest.id} ${label} overlaps the resident launcher data`);
    }
    check(!overlaps(codeBase, pageUp(code.length), dataBase, dataBytes),
      `app ${pkg.manifest.id} code and data regions overlap`);

    const started = Date.now();
    let codeSource = 0, dataSource = 0, heap = 0, stack = 0, assetsBlock = 0, saveBlock = 0;
    let importBlock = 0;
    let exclusive = false;
    let appStatus = 0;
    try {
      check(data.length <= dataBytes, `${pkg.manifest.id}: data.bin does not fit its region`);
      /* One slack page: the bulk unmap rejects exactly page-sized sources. */
      codeSource = await this.alloc(`${pkg.manifest.id}-code-source`, code.length + PAGE);
      await this.target.write(codeSource, code);
      await this.mapImage(pkg.manifest.id + '-code', {source: codeSource,
        destination: codeBase, bytes: code.length, permission: 5});
      await this.target.flushCode(codeBase, code);

      dataSource = await this.alloc(`${pkg.manifest.id}-data-source`, dataBytes + PAGE);
      /* The image expects a zero-filled data region (crt0 does not zero .bss);
         session allocations are not guaranteed to be zero. */
      {
        const region = Buffer.alloc(dataBytes);
        if (data.length) data.copy(region, 0, 0, Math.min(data.length, dataBytes));
        await this.target.write(dataSource, region);
      }
      await this.mapImage(pkg.manifest.id + '-data', {source: dataSource,
        destination: dataBase, bytes: dataBytes, permission: 3});

      const heapBytes = pkg.limits.heap_bytes;
      const stackBytes = pkg.limits.stack_bytes;
      heap = await this.target.allocate(heapBytes, 0x1000);
      stack = await this.target.allocate(stackBytes, 0x1000);
      const assets = this.buildAssetBlock(pkg);
      assetsBlock = await this.target.allocate(assets.length, 0x1000);
      await this.target.write(assetsBlock, assets);
      if (pkg.saveBytes) saveBlock = await this.loadSave(pkg);
      if (pkg.imports.length) importBlock = await this.resolveImports(pkg);
      if (pkg.manifest.display === 'exclusive' && this.target.kind === 'live') {
        await this.releaseLauncherDisplay();
        exclusive = true;
      }

      const ctx = Ctx.header()
        .u32('flags', FLAG_NO_AV)
        .u64('status', 0).u64('exit', 0).u64('frame', 0)
        .u64('now_ns', this.elapsedNs()).u64('tick_hz', this.tickHz || 0).u64('uptime_ns', this.elapsedNs())
        .u64('heap_base', heap).u64('heap_size', heapBytes)
        .u64('data_base', assetsBlock).u64('data_size', assets.length)
        .u64('code_base', codeBase).u64('code_size', code.length)
        .u64('stack_base', stack).u64('stack_size', stackBytes)
        .u64('control', this.control)
        .u64('catalog', this.catalogBlock).u64('catalog_bytes', this.catalog.block.length)
        .u64('icons', this.iconsBlock || 0)
      .u64('icons_bytes', this.catalog.icons.length)
        .u64('assets', assetsBlock)
        .u64('log', this.logStruct)
        .u64('ui', this.shared.ui)
        .u64('atlas', this.atlasBlock).u64('atlas_bytes', 1024 * 1024);
      if (this.shared.gpu)
        this.shared.gpu.copy(ctx.buffer, ABI.ctx.gpu, 0, ABI.gpuSize);
      if (exclusive) {
        /* Exclusive display: the launcher released its surface and swapchain;
           the app builds its own on the same vi window (ctx gpu.vi_window). */
        ctx.buffer.writeBigUInt64LE(0n, ABI.ctx.gpu + ABI.gpu.surface);
        ctx.buffer.writeBigUInt64LE(0n, ABI.ctx.gpu + ABI.gpu.swapchain);
      }
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.pad_handheld || 0), ABI.ctx.input);
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.pad_full_key || 0), ABI.ctx.input + 8);
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.pad_joy_dual || 0), ABI.ctx.input + 16);
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.pad_joy_left || 0), ABI.ctx.input + 24);
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.pad_joy_right || 0), ABI.ctx.input + 32);
      ctx.buffer.writeBigUInt64LE(BigInt(this.input.touch || 0), ABI.ctx.input + 40);
      ctx.buffer.writeBigUInt64LE(BigInt(this.padState), ABI.ctx.input + 48);
      ctx.buffer.writeBigUInt64LE(BigInt(this.touchState), ABI.ctx.input + 56);
      ctx.buffer.writeBigUInt64LE(BigInt(pkg.limits.frames), ABI.ctx.args + 24);
      this.writeAudio(ctx);
      if (options.stage)
        ctx.buffer.writeBigUInt64LE(BigInt(options.stage), ABI.ctx.args + 8);
      /* args[4] = 1 asks an app for its on-screen diagnostic HUD (NES). */
      if (process.env.HBL_APP_HUD === '1')
        ctx.buffer.writeBigUInt64LE(1n, ABI.ctx.args + 32);
      /* args[0]: SDK import table {u64 count; u64 address[count]} in manifest order. */
      if (importBlock) ctx.buffer.writeBigUInt64LE(BigInt(importBlock), ABI.ctx.args);
      if (saveBlock) {
        ctx.buffer.writeBigUInt64LE(BigInt(saveBlock), ABI.ctx.args + 48);
        ctx.buffer.writeBigUInt64LE(BigInt(pkg.saveBytes), ABI.ctx.args + 56);
      }

      await this.target.write(this.ctxBlock, ctx.buffer);
      const threaded = !!report.spawn && this.target.kind === 'live' &&
        process.env.HBL_BLOCKING_APPS !== '1';
      const reply = threaded
        ? await this.runThreaded(pkg, Number(BigInt(report.spawn)), code, codeBase)
        : await this.callEntry(this.target, Number(BigInt(report.entry)), code, [this.ctxBlock]);
      const contextStatus = Number((await this.target.read(this.ctxBlock + ABI.ctx.status, 8))
        .readBigUInt64LE() & (threaded ? 0xffffffffn : 0xffffffffffffffffn));
      const frames = Number((await this.target.read(this.ctxBlock + ABI.ctx.frame, 8))
        .readBigUInt64LE());
      /* The ABI returns the status in w0; apps are not required to mirror it
         into ctx->status, so prefer the context when it is non-zero. */
      appStatus = contextStatus !== 0 ? contextStatus : (reply.result >>> 0);
      this.log.info('app finished', {id: pkg.manifest.id, status: appStatus,
        frames, reply: reply.result, loadMs: Date.now() - started});
      await this.logProgress(pkg.manifest.id);
    } catch (error) {
      appStatus = error.code === 'abi' ? 2 : 9; /* HBX_ERR_ABI / HBX_ERR_INTERNAL */
      this.log.error('app launch failed', {id: pkg.manifest.id, error: error.message,
        code: error.code});
    } finally {
      await this.readLog();
      if (saveBlock) {
        try { await this.storeSave(pkg, saveBlock); }
        catch (error) { this.log.warn('save block not stored: ' + error.message); }
      }
      await this.release({labels: [pkg.manifest.id + '-code', pkg.manifest.id + '-data'],
        codeSource, dataSource, heap, stack, assetsBlock, saveBlock, importBlock});
    }
    return appStatus;
  }

  /* SDK imports: resolve manifest "imports" against the audited SDK export
   * table (sdk/sdk-symbols.json) and require each address to be executable.
   * All or nothing: a wrong entry would execute garbage. */
  async resolveImports(pkg) {
    if (!this.sdkSymbols)
      this.sdkSymbols = readJson(path.join(ROOT, 'sdk', 'sdk-symbols.json')).functions;
    check(this.target.kind === 'live' || this.target.kind === 'mock', 'imports need a target');
    const sdk = this.target.kind === 'live' ? Number(this.target.rpc.a.sdk) : 0x7000000000;
    const table = Buffer.alloc(8 * (pkg.imports.length + 1));
    table.writeBigUInt64LE(BigInt(pkg.imports.length), 0);
    const pages = new Map();
    for (const [index, name] of pkg.imports.entries()) {
      const offset = this.sdkSymbols[name];
      check(Number.isSafeInteger(offset), `${pkg.manifest.id}: unknown SDK import ${name}`);
      const address = sdk + offset;
      if (this.target.kind === 'live') {
        const page = Math.floor(address / PAGE);
        if (!pages.has(page)) {
          const region = await this.target.query(address);
          pages.set(page, (Number(region.permissions) & 5) === 5);
        }
        check(pages.get(page), `${pkg.manifest.id}: import ${name} is not executable`);
      }
      table.writeBigUInt64LE(BigInt(address), 8 * (index + 1));
    }
    const block = await this.target.allocate(table.length, 0x1000);
    await this.target.write(block, table);
    this.log.info('imports resolved', {id: pkg.manifest.id, count: pkg.imports.length});
    return block;
  }

  /* Persistent save block: preload saves/<id>.sav (PC side) into a fresh RAM
   * block; an absent or oversized file starts the app with an empty header. */
  saveFile(pkg) {
    const dir = process.env.HBL_SAVES_DIR || path.join(ROOT, 'saves');
    return path.join(dir, pkg.manifest.id + '.sav');
  }

  async loadSave(pkg) {
    const block = await this.target.allocate(pkg.saveBytes, 0x1000);
    const file = this.saveFile(pkg);
    let bytes = null;
    try { bytes = fs.readFileSync(file); } catch { bytes = null; }
    if (bytes && bytes.length >= 16 && bytes.length <= pkg.saveBytes) {
      await this.target.write(block, bytes);
      this.log.info('save block loaded', {id: pkg.manifest.id, bytes: bytes.length});
    } else {
      await this.target.write(block, Buffer.alloc(16));
      if (bytes) this.log.warn('save file ignored (size)', {file, bytes: bytes.length});
    }
    return block;
  }

  async storeSave(pkg, block) {
    const header = await this.target.read(block, 16);
    const magic = header.readUInt32LE(0);
    const used = header.readUInt32LE(8);
    const dirty = header.readUInt32LE(12);
    if (magic !== 0x56534248 || !dirty) return;
    check(used >= 16 && used <= pkg.saveBytes, `save block size ${used} out of range`);
    const bytes = await this.target.read(block, used);
    bytes.writeUInt32LE(0, 12);
    const file = this.saveFile(pkg);
    fs.mkdirSync(path.dirname(file), {recursive: true});
    if (fs.existsSync(file)) fs.copyFileSync(file, file + '.bak');
    fs.writeFileSync(file + '.tmp', bytes);
    fs.renameSync(file + '.tmp', file);
    this.log.info('save block stored', {id: pkg.manifest.id, bytes: used, file});
  }

  /* Exclusive apps bring their own swapchain on the launcher's vi window.
   * One short launcher entry with HBX_CTL_FLAG_RELEASE_DISPLAY frees the
   * launcher's surface and swapchain; its next normal entry rebuilds them.
   * (A second vi layer was tried first: hiding/closing layers crashed or
   * wedged SmileBASIC.) */
  async releaseLauncherDisplay() {
    const flags = (await this.target.read(this.control + 20, 4)).readUInt32LE(0);
    const word = Buffer.alloc(4);
    word.writeUInt32LE((flags | 0x10) >>> 0);
    await this.target.write(this.control + 20, word);
    const ctx = this.fillLauncherCtx({sequence: 1, maxFrames: 1});
    await this.target.write(this.ctxBlock, ctx.buffer);
    await this.callEntry(this.target, this.launcher.entry, this.launcher.code, [this.ctxBlock]);
    const after = (await this.target.read(this.control + 20, 4)).readUInt32LE(0);
    check(!(after & 0x10), 'the launcher did not release its display');
    this.log.info('launcher display released for an exclusive app');
  }

  /* A dedicated vi layer for apps that bring their own swapchain (Linux
   * personality: Dawn).  The launcher's layer is hidden meanwhile. */
  async openAppLayer() {
    const viLayer = require('./vi-layer.cjs');
    const record = await viLayer.execute({run: this.runDir, force: true, file: 'vi-layer-app.json'});
    check(record && record.created, 'app vi layer was not created');
    /* Hiding the launcher layer crashed SmileBASIC once (unconfirmed); opt in. */
    if (process.env.HBL_HIDE_LAUNCHER_LAYER === '1') await this.setLayerVisible(this.launcherLayer, false);
    this.log.info('app vi layer ready', {window: hex(record.window), layer: hex(record.layer)});
    return record;
  }

  async closeAppLayer() {
    const viLayer = require('./vi-layer.cjs');
    try { await viLayer.closeLayer({run: this.runDir, file: 'vi-layer-app.json'}); }
    finally {
      if (process.env.HBL_HIDE_LAUNCHER_LAYER === '1') await this.setLayerVisible(this.launcherLayer, true);
    }
  }

  async setLayerVisible(record, visible) {
    if (!record || !record.layer) return;
    if (!this.sdkSymbols)
      this.sdkSymbols = readJson(path.join(ROOT, 'sdk', 'sdk-symbols.json')).functions;
    const fn = this.sdkSymbols['_ZN2nn2vi18SetLayerVisibilityEPNS0_5LayerEb'];
    if (fn) await this.target.rpc.call(fn, [record.layer, visible ? 1 : 0, 0]);
  }

  /* Threaded launch: one short bridge call creates an nn::os thread that runs
   * the image (crt0 _spawn -> hbx_spawn), then the host polls ctx->status
   * with small reads and streams the app's log as it goes.  Minutes-long
   * bridge calls lost their reply on hardware; these never block the bridge. */
  async runThreaded(pkg, spawn, code, codeBase) {
    if (!this.sdkSymbols)
      this.sdkSymbols = readJson(path.join(ROOT, 'sdk', 'sdk-symbols.json')).functions;
    const fn = this.sdkSymbols;
    const rpc = this.target.rpc;
    if (this.appThreadPriority === undefined) {
      const current = await rpc.call(fn.nnosGetCurrentThread, [0, 0, 0]);
      const priority = await rpc.call(fn.nnosGetThreadPriority, [Number(current.result), 0, 0]);
      this.appThreadPriority = Number(BigInt.asIntN(32, priority.result));
      this.log.info('app thread priority', {priority: this.appThreadPriority});
    }
    const sdk = Number(rpc.a.sdk);
    const thread = await this.target.allocate(0x1000, 0x1000);
    const stack = await this.target.allocate(0x10000, 0x1000);
    const params = await this.target.allocate(0x1000, 0x1000);
    try {
      await this.target.write(thread, Buffer.alloc(0x1000));
      const p = Buffer.alloc(48);
      p.writeBigUInt64LE(BigInt(sdk + fn.nnosCreateThreadWithCoreNumber), 0);
      p.writeBigUInt64LE(BigInt(sdk + fn.nnosStartThread), 8);
      p.writeBigUInt64LE(BigInt(thread), 16);
      p.writeBigUInt64LE(BigInt(stack), 24);
      p.writeBigUInt64LE(0x10000n, 32);
      p.writeInt32LE(this.appThreadPriority, 40);
      p.writeInt32LE(1, 44);                                   /* core 1; SmileBASIC is on 0 */
      await this.target.write(params, p);
      await this.target.verify(spawn, code.subarray(spawn - codeBase));
      const reply = await this.target.call(spawn, [params, this.ctxBlock]);
      check(reply.result === 0, `${pkg.manifest.id}: thread creation failed (${hex(Number(BigInt(reply.raw) & 0xffffffffn))})`);
      this.log.info('app thread started', {id: pkg.manifest.id});
      const head = await this.target.read(this.logStruct + 12, 4);
      this.logHead = head.readUInt32LE(0);
      /* Between RPC calls SmileBASIC's interpreter runs, and with it its own
       * input handling: the sticks move its cursor and it beeps.  Exclusive
       * apps own the screen and controller, so park SmileBASIC's thread in a
       * native sleep instead of idling it in BASIC between polls. */
      const quiet = pkg.manifest.display === 'exclusive';
      const sleepThread = fn['_ZN2nn2os11SleepThreadENS_8TimeSpanE'];
      /* SIGUSR1 (tools/console.py stop) asks the app to exit: control flags
       * bit 0, the stop request native apps already honour. */
      let stopSent = false;
      this.stopRequested = false;
      const onStop = () => { this.stopRequested = true; };
      process.on('SIGUSR1', onStop);
      let status;
      for (;;) {
        status = (await this.target.read(this.ctxBlock + ABI.ctx.status, 8)).readBigUInt64LE();
        if (status >> 32n) break;
        if (this.stopRequested && !stopSent && this.control) {
          const flags = (await this.target.read(this.control + 20, 4)).readUInt32LE(0);
          const word = Buffer.alloc(4);
          word.writeUInt32LE((flags | 1) >>> 0);
          await this.target.write(this.control + 20, word);
          stopSent = true;
          this.log.info('stop requested', {id: pkg.manifest.id});
        }
        await this.streamLog();
        if (quiet) await rpc.call(sleepThread, [1000000000, 0, 0]);
        else await sleep(100);
      }
      process.off('SIGUSR1', onStop);
      if (stopSent) {
        const flags = (await this.target.read(this.control + 20, 4)).readUInt32LE(0);
        const word = Buffer.alloc(4);
        word.writeUInt32LE((flags & ~1) >>> 0);
        await this.target.write(this.control + 20, word);
      }
      await this.streamLog();
      await rpc.call(fn.nnosWaitThread, [thread, 0, 0]);
      await rpc.call(fn.nnosDestroyThread, [thread, 0, 0]);
      return {result: Number(status & 0xffffffffn), raw: status};
    } finally {
      for (const block of [params, stack, thread]) {
        try { await this.target.free(block); } catch { /* best effort */ }
      }
    }
  }

  /* Print log ring text written since the last call. */
  async streamLog() {
    try {
      const head = (await this.target.read(this.logStruct + 12, 4)).readUInt32LE(0);
      if (head === this.logHead) return;
      let bytes;
      if (head > this.logHead) bytes = await this.target.read(this.logRing + this.logHead, head - this.logHead);
      else bytes = Buffer.concat([await this.target.read(this.logRing + this.logHead, this.logRingSize - this.logHead),
                                  await this.target.read(this.logRing, head)]);
      this.logHead = head;
      const text = bytes.toString('utf8').replace(/\0/g, '').trimEnd();
      if (text) this.log.info('app', {text});
    } catch { /* best effort */ }
  }

  async release({labels, codeSource, dataSource, heap, stack, assetsBlock, saveBlock, importBlock}) {
    /* Unmap the app images first, then free the source blocks they came from. */
    const stuck = labels ? await this.releaseImages(labels) : new Set();
    for (const block of [codeSource, dataSource, heap, stack, assetsBlock, saveBlock, importBlock]) {
      if (!block || stuck.has(block)) continue;
      try { await this.target.free(block); }
      catch (error) { this.log.warn('free failed: ' + error.message); }
    }
  }

  /*
   * Default teardown keeps every block mapped.  The launcher is a resident
   * service for the SmileBASIC session: its code, data and renderer hold
   * pointers into the context, atlas, catalog, icons and dispatch blocks, and
   * the vi swapchain is retained by design.  Freeing them would leave dangling
   * handles and make a second launch create a second display.  The process
   * reclaims everything when SmileBASIC is restarted.
   *
   * `teardown: true` unmaps the launcher images and frees the blocks for a
   * deliberate cold stop (the next run remaps and recreates the display).
   */
  async stop(options = {}) {
    if (this.launcher && this.launcher.resident) {
      /* A resident launcher (from an earlier hbl-start in this SmileBASIC
         session) owns the display and holds pointers into every shared block;
         freeing them would leave dangling handles.  Only a SmileBASIC restart
         reclaims them. */
      this.log.info('launcher is resident; nothing can be freed safely. ' +
        'Restart SmileBASIC for a cold start.');
      await this.target.close();
      return;
    }
    if (!options.teardown) {
      this.log.info('resources retained (resident launcher); restart SmileBASIC to reclaim them');
      await this.target.close();
      return;
    }
    await this.releaseImages(['launcher-code', 'launcher-data']);
    for (const block of [...this.blocks].reverse()) {
      try { await this.target.free(block.base); } catch { /* ignored */ }
    }
    this.blocks = [];
    await this.target.close();
  }
}

module.exports = {Runtime, Ctx, ABI, LAUNCHER_CODE_BASE, LAUNCHER_DATA_BASE, EXIT};
