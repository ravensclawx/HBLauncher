'use strict';
/*
 * target.cjs - the two backends the launcher runtime can talk to.
 *
 *   LiveTarget  - the real console session (ns2jb-kurumuz run directory)
 *   MockTarget  - an in-process address space used by the offline tests
 *
 * Both expose the same small surface:
 *   prepare() allocate() free() write() read() query()
 *   mapImage() unmapImage() flushCode() call() placeData()
 *
 * ns2jb (kurumuz fork, commit 81d5bb1) accepts command IDs 0x524f0010
 * through 0x524f0016 only.  The bulk range commands used before
 * (map RX range, map RW range, unmap range) are rejected, so images are
 * mapped one page at a time with CMD_MAP_PAGE
 * (0x524f0012: page-aligned Normal/RW source, free destination, exactly
 * one page per call; the payload finalises the destination RX itself) and
 * released one page at a time with CMD_UNMAP_PAGE (0x524f0013).
 *
 * Nothing above this layer knows which backend it is talking to, so the whole
 * lifecycle (map launcher -> frame loop -> launch app -> return) is exercised
 * by tests/host/lifecycle.test.cjs without a console.
 */
const path = require('node:path');
const {HostError, check, hex} = require('./common.cjs');

const PAGE = 0x1000;
const HANDLE = 0xffff8001;
/* ns2jb (kurumuz fork, commit 81d5bb1) accepts 0x524f0010-0x524f0016 only:
 * CMD_MAP_PAGE maps one page from a page-aligned Normal/RW source into a free
 * destination and finalises it RX (process.c 0x524f0012), CMD_UNMAP_PAGE
 * releases one page (0x524f0013) and CMD_PATCH writes 16 bytes with cache
 * publish and readback (0x524f0014).  The bulk range map/unmap commands
 * used before are not implemented by any published ns2jb and are never sent. */
const CMD_MAP_PAGE = 0x524f0012;
const CMD_UNMAP_PAGE = 0x524f0013;
const CMD_PATCH = 0x524f0014;
const SDK_ALLOCATE = 5732112;
/* operator delete(void*) -> free().  5797280 (used before) is memset, which
   made every release a silent no-op and leaked each launch's blocks. */
const SDK_FREE = 5616480;

class LiveTarget {
  constructor(options) {
    this.runDir = options.runDir;
    this.log = options.log;
    this.pid = 0;
    this.prepared = null;
    this.mapped = [];
  }

  static async open(options) {
    const target = new LiveTarget(options);
    const runDir = target.runDir;
    target.rpc = require(path.join(runDir, 'shared/rpc.cjs'));
    const {connect} = require(path.join(runDir, 'switch2/tools/fs-memory.cjs'));
    const {Client} = require(path.join(runDir, 'switch2/process/client.cjs'));
    target.memory = await connect();
    target.client = await new Client().open();
    /* Page-by-page unmap makes thousands of calls; keep the audit journal in memory. */
    target.client.save = async () => {};
    const app = await target.client.info(HANDLE);
    check(!Number(app.result) && Number(app.pid) > 0, 'cannot identify the application process');
    target.pid = Number(app.pid);
    target.freeAddress = Number(app.freeAddress);
    target.session = String(target.rpc.a.session);
    return target;
  }

  get kind() { return 'live'; }
  get scratchOutput() { return this.rpc.output; }

  async prepare() {
    this.prepared = await this.memory.request({command: 'root-native-process-test-prepare'});
    check(Number(this.prepared.output) > 0, 'prepared context page is invalid');
    return this.prepared;
  }

  async allocate(size, align = PAGE) {
    check(Number.isInteger(size) && size > 0, 'allocation size');
    const reply = await this.rpc.call(SDK_ALLOCATE, [this.rpc.output, align, size]);
    check(reply.result === 0n, 'SDK allocate failed: ' + reply.result);
    const base = Number(reply.output.readBigUInt64LE());
    check(Number.isSafeInteger(base) && base > 0 && base % PAGE === 0, 'bad allocation address');
    return base;
  }

  async free(address) {
    /* free() returns void: x0 is whatever the callee left there. */
    await this.rpc.call(SDK_FREE, [address, 0, 0]);
    return true;
  }

  async write(address, buffer) { await this.rpc.write(address, buffer); }
  async read(address, length) { return this.rpc.read(address, length); }
  async query(address) { return this.rpc.query(address); }

  /* The bridge refuses call-address until the entry has been verified against
     the bytes at the destination. */
  async verify(address, bytes) {
    const reply = await this.rpc.request({command: 'verify-function', address,
      hex: bytes.subarray(0, 32).toString('hex')});
    check(reply.verified, 'entry verification failed at ' + hex(address));
  }

  async call(address, args) {
    const reply = await this.rpc.callAddress(address, args);
    return {result: Number(BigInt.asIntN(32, reply.result)), raw: reply.result};
  }

  /* ns2jb publishes only a single-page map (0x524f0012): the source must be a
   * page-aligned Normal/RW allocation, the destination free, the size exactly
   * PAGE.  The payload finalises the destination RX itself (process_permission
   * 5) and rolls a failed page back on its own; this loop rolls back the pages
   * mapped before the failure so the destination is left free. */
  async mapRange({source, destination, pages, permission}) {
    check(pages > 0, 'map page count ' + pages);
    check(source % PAGE === 0 && destination % PAGE === 0, 'map range not page aligned');
    const mapped = [];
    for (let page = 0; page < pages; page++) {
      const at = destination + page * PAGE;
      const reply = await this.client.request(CMD_MAP_PAGE, HANDLE,
        [this.pid, source + page * PAGE, at, PAGE]);
      const v = reply.values.map(Number);
      if (Number(reply.result) || v[3] || v[4]) {
        for (const done of mapped) {
          await this.client.request(CMD_UNMAP_PAGE, HANDLE,
            [this.pid, source + done * PAGE, destination + done * PAGE, PAGE]).catch(() => {});
        }
        throw new HostError(`map page ${hex(at)} failed (result ${Number(reply.result)}, ` +
          `mapResult ${v[3] || 0}, permissionResult ${v[4] || 0}, ` +
          `rollbackResult ${v[5] || 0})`, 'map');
      }
      mapped.push(page);
    }
    return {mapped: mapped.length, mode: permission};
  }

  /* ns2jb has no bulk range unmap; release pages one at a time with
   * CMD_UNMAP_PAGE, which it accepts. */
  async unmapRange({source, destination, pages}) {
    let lastError = 0;
    let page = 0;
    for (; page < pages; page++) {
      const reply = await this.client.request(CMD_UNMAP_PAGE, HANDLE,
        [this.pid, source + page * PAGE, destination + page * PAGE, PAGE]);
      if (Number(reply.result)) { lastError = Number(reply.result); break; }
    }
    if (lastError) {
      throw new Error(`unmap page-by-page failed at page ${page}, result ${lastError}`);
    }
    return {unmapped: pages};
  }

  /* Map one contiguous image into one contiguous free destination, one
     page per 0x524f0012 call.  The destination must be free: the payload
     query_processes it and fails when target.type != 0.  The payload also
     finalises every destination page RX (process_permission 5 is hardcoded
     inside the command), so mapImage is an RX-only primitive: asking for a
     writable mapping here would silently produce RX pages whose first write
     faults.  Writable image state is placed with placeData() instead. */
  async mapImage({source, destination, bytes, permission}) {
    check(permission === 5, 'mapImage can only produce RX pages ' +
      '(the payload hardcodes process_permission 5 inside 0x524f0012); ' +
      `place writable data with placeData(), got permission ${permission}`);
    const pages = Math.ceil(bytes / PAGE);
    check(pages > 0 && pages <= 2048, 'map page count ' + pages +
      ' at ' + hex(destination) + ' (relink at a different --code-base)');
    const region = await this.query(destination);
    /* region.size counts from region.address, not from the destination. */
    const room = Number(region.address) + Number(region.size) - destination;
    check(Number(region.type) === 0 && room >= pages * PAGE,
      `destination ${hex(destination)} is not free (${JSON.stringify(region)}). ` +
      'A stranded previous app image or another mapping (e.g. the exploit ' +
      'helper) already owns this base; rebuild the app at a different ' +
      '--code-base, or restart SmileBASIC if a previous launch leaked it.');
    const record = {source, destination, pages, permission, bytes};
    this.mapped.push(record);
    try {
      await this.mapRange({source, destination, pages, permission});
    } catch (error) {
      this.mapped = this.mapped.filter(item => item !== record);
      throw error;
    }
    return record;
  }

  /* Cache maintenance: one patch per 64-byte line flushes and invalidates the
     whole line, so a 64-byte stride covers the image. */
  async flushCode(destination, image) {
    /* The single-page map's permission change publishes the new code: a
     * launcher mapped without any per-line publish ran 60 frames correctly on
     * hardware (2026-10-28), and 0x524f0012 also calls process_permission.
     * Set HBL_PUBLISH=1 to force the per-line flush (e.g. after rewriting code
     * at an address whose lines may still be cached). */
    if (process.env.HBL_PUBLISH !== '1') return {calls: 0, skipped: true};
    let calls = 0;
    /* Only addresses with a full 16-byte window exist in the image; the tail
       of the last line shares a cache line with the previous publish. */
    for (let offset = 0; offset + 16 <= image.length; offset += 64) {
      const address = destination + offset;
      const block = image.subarray(offset, offset + 16);
      const reply = await this.client.patch(HANDLE, this.pid, address, block, block);
      check(!Number(reply.result) && reply.expectedMatched && reply.readbackMatched,
        'code publish failed at ' + hex(address));
      calls++;
    }
    if (image.length < 16 && image.length > 0) {
      /* Tiny test images: publish a zero-padded window at the start. */
      const block = Buffer.alloc(16);
      image.copy(block, 0, 0, image.length);
      const reply = await this.client.patch(HANDLE, this.pid, destination, block, block);
      check(!Number(reply.result) && reply.expectedMatched && reply.readbackMatched,
        'code publish failed at ' + hex(destination));
      calls++;
    }
    if (image.length >= 16) {
      /* Ensure the final partial line is published at least once.  The patch
       * command requires a 4-byte-aligned address, so the window is aligned
       * down; it always stays inside the image. */
      const last = (image.length - 16) & ~3;
      if (last % 64 !== 0) {
        const address = destination + last;
        const block = image.subarray(last, last + 16);
        const reply = await this.client.patch(HANDLE, this.pid, address, block, block);
        check(!Number(reply.result) && reply.expectedMatched && reply.readbackMatched,
          'code publish failed at ' + hex(address));
        calls++;
      }
    }
    return {calls};
  }

  async unmapImage(entry) {
    const pages = entry.pages || Math.ceil((entry.bytes || 0) / PAGE);
    check(pages > 0, 'unmap needs a page count for ' + hex(entry.destination));
    /* ns2jb's only unmap form is the single-page command (0x524f0013), so
     * release the image one page at a time.  A failure leaves the pages in
     * place; the caller keeps the source block and reports it. */
    await this.unmapRange({source: entry.source, destination: entry.destination, pages});
    this.mapped = this.mapped.filter(item =>
      !(item.destination === entry.destination && item.source === entry.source));
  }

  /* Place an image's writable region: an SDK-allocated Normal/RW block,
     written directly.  This is the only way to give an image writable memory
     - the payload has no RW map - so the block address is handed to the
     image through ctx (data_base/data_size) instead of a link-time base.
     `bytes` is the region size the image needs; `content` (optional) is
     written at offset 0 (data.bin).  The rest stays zero: SDK allocations
     are not guaranteed zero, so the whole block is written first. */
  async placeData({bytes, content}) {
    check(Number.isInteger(bytes) && bytes > 0, 'data region size ' + bytes);
    check(!content || content.length <= bytes, 'data content exceeds its region');
    const region = Buffer.alloc(bytes);
    if (content && content.length) content.copy(region, 0, 0, Math.min(content.length, bytes));
    const block = await this.allocate(bytes);
    await this.write(block, region);
    const info = await this.query(block);
    check(Number(info.type) === 5 && Number(info.permissions) === 3,
      'data block is not Normal/RW: ' + JSON.stringify(info));
    return block;
  }

  async close() {
    try { await this.client.close(); } catch (error) { this.log?.warn('client close: ' + error.message); }
  }
}

class MockTarget {
  constructor(options = {}) {
    this.log = options.log;
    this.session = 'mock-session';
    this.pid = 4242;
    this.freeAddress = 0x40000000;
    this.heapTop = 0x100000000;
    this.regions = [];
    this.calls = [];
    this.onCall = options.onCall || (() => ({result: 0}));
    this.mappedRanges = [];
    this.patchCalls = 0;
    this.log?.info('mock target ready (no console)');
  }

  get kind() { return 'mock'; }

  async prepare() { this.prepared = {source: 0x20000000, output: 0x2000f000}; return this.prepared; }

  async allocate(size, align = PAGE) {
    /* Arithmetic, not bitwise: addresses exceed 2^31. */
    const base = Math.ceil(this.heapTop / align) * align;
    this.heapTop = base + size + PAGE;
    this.regions.push({address: base, size, type: 5, permissions: 3, data: Buffer.alloc(size)});
    return base;
  }
  async free(address) {
    const index = this.regions.findIndex(region => region.address === address);
    if (index >= 0) this.regions.splice(index, 1);
    return index >= 0;
  }
  regionOf(address, length = 1) {
    return this.regions.find(region =>
      address >= region.address && address + length <= region.address + region.size);
  }
  async write(address, buffer) {
    let remaining = buffer;
    let at = address;
    while (remaining.length) {
      const region = this.regionOf(at);
      check(region, 'mock write outside any region at ' + hex(at));
      const offset = at - region.address;
      const size = Math.min(remaining.length, region.size - offset);
      remaining.copy(region.data, offset, 0, size);
      at += size;
      remaining = remaining.subarray(size);
    }
  }
  async read(address, length) {
    const region = this.regionOf(address, length);
    check(region, 'mock read outside any region at ' + hex(address));
    return Buffer.from(region.data.subarray(address - region.address, address - region.address + length));
  }
  async query(address) {
    const region = this.regions.find(item =>
      address >= item.address && address < item.address + item.size);
    if (region) return {type: region.type, size: region.size, permissions: region.permissions};
    return {type: 0, size: 0x10000000, permissions: 0};
  }
  /* Mirrors the payload's 0x524f0012 checks: page-aligned source and
     destination, source Normal/RW and covering every mapped page, free
     destination.  The payload finalises the destination RX (permission 5). */
  async mapImage({source, destination, bytes, permission}) {
    check(permission === 5, 'mapImage is RX-only; use placeData for writable data');
    const pages = Math.ceil(bytes / PAGE);
    check(pages > 0, 'mock map needs bytes');
    check(source % PAGE === 0 && destination % PAGE === 0,
      'mock map needs page-aligned source and destination');
    const sourceRegion = this.regionOf(source, pages * PAGE);
    check(sourceRegion, 'mock map source does not cover ' + pages + ' page(s)');
    check(sourceRegion.type === 5 && sourceRegion.permissions === 3,
      'mock map source is not a Normal/RW allocation');
    const occupied = this.regions.some(region =>
      destination < region.address + region.size &&
      region.address < destination + pages * PAGE);
    check(!occupied, 'mock map destination is not free');
    const copy = Buffer.from(sourceRegion.data.subarray(source - sourceRegion.address,
      source - sourceRegion.address + pages * PAGE));
    this.regions.push({address: destination, size: pages * PAGE, type: 5,
      permissions: 5, data: copy, mapped: true});
    this.mappedRanges.push({source, destination, pages, permission});
    return {pages};
  }
  async unmapImage(entry) {
    const index = this.regions.findIndex(region => region.address === entry.destination &&
      region.mapped);
    if (index >= 0) this.regions.splice(index, 1);
  }
  /* Mirrors LiveTarget.placeData: an RW allocation written directly. */
  async placeData({bytes, content}) {
    check(Number.isInteger(bytes) && bytes > 0, 'data region size ' + bytes);
    const region = Buffer.alloc(bytes);
    if (content && content.length) content.copy(region, 0, 0, Math.min(content.length, bytes));
    const block = await this.allocate(bytes);
    await this.write(block, region);
    return block;
  }
  async flushCode(destination, image) {
    this.patchCalls += Math.ceil(image.length / 64);
    return {calls: Math.ceil(image.length / 64)};
  }
  async call(address, args) {
    this.calls.push({address, args});
    const result = await this.onCall(address, args, this);
    return result || {result: 0};
  }
  async close() {}
}

module.exports = {LiveTarget, MockTarget, PAGE, HANDLE, SDK_ALLOCATE, SDK_FREE,
  CMD_MAP_PAGE, CMD_UNMAP_PAGE, CMD_PATCH};
