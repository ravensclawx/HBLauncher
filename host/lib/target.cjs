'use strict';
/*
 * target.cjs - the two backends the launcher runtime can talk to.
 *
 *   LiveTarget  - the real console session (ns2jb-kurumuz run directory)
 *   MockTarget  - an in-process address space used by the offline tests
 *
 * Both expose the same small surface:
 *   prepare() allocate() free() write() read() query()
 *   mapImage() unmapImage() flushCode() call()
 *
 * Nothing above this layer knows which backend it is talking to, so the whole
 * lifecycle (map launcher -> frame loop -> launch app -> return) is exercised
 * by tests/host/lifecycle.test.cjs without a console.
 */
const path = require('node:path');
const {HostError, check, hex} = require('./common.cjs');

const PAGE = 0x1000;
const HANDLE = 0xffff8001;
const CMD_MAP_RX_RANGE = 0x524f001a;
const CMD_MAP_RW_RANGE = 0x524f001b;
const CMD_UNMAP_RANGE = 0x524f001c;
const CMD_UNMAP_PAGE = 0x524f0013;
const CMD_PATCH = 0x524f0014;
const SDK_ALLOCATE = 5732112;
/* operator delete(void*) -> free().  5797280 (used before) is memset, which
   made every release a silent no-op and leaked each launch's blocks. */
const SDK_FREE = 5616480;
const MAX_RANGE_PAGES = 16384;

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
    /* Bulk mapping makes thousands of calls; keep the audit journal in memory. */
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

  async mapRange({source, destination, pages, permission}) {
    check(pages > 0 && pages <= MAX_RANGE_PAGES, 'range page count ' + pages);
    const id = permission === 5 ? CMD_MAP_RX_RANGE : CMD_MAP_RW_RANGE;
    const reply = await this.client.request(id, HANDLE, [this.pid, source, destination, pages]);
    const v = reply.values.map(Number);
    const mapped = v[3] || 0;
    check(!Number(reply.result) && mapped === pages,
      `map ${permission === 5 ? 'RX' : 'RW'} ${hex(destination)} failed ` +
      `(${mapped}/${pages} pages, result ${Number(reply.result)})`);
    return {mapped, mode: v[8] || 0};
  }

  async unmapRange({source, destination, pages}) {
    const reply = await this.client.request(CMD_UNMAP_RANGE, HANDLE,
      [this.pid, source, destination, pages]);
    const v = reply.values.map(Number);
    check(!Number(reply.result) && !v[6],
      `unmap ${hex(destination)} failed (result ${Number(reply.result)}, ` +
      `unmapped ${v[3]}/${pages}, failPage ${v[4]}, failResult ${v[5]}, ` +
      `failures ${v[6]}, source ${hex(source)})`);
    return {unmapped: v[3] || 0};
  }

  /* Map one contiguous image into one contiguous free destination. */
  async mapImage({source, destination, bytes, permission}) {
    const pages = Math.ceil(bytes / PAGE);
    const region = await this.query(destination);
    /* region.size counts from region.address, not from the destination. */
    const room = Number(region.address) + Number(region.size) - destination;
    check(Number(region.type) === 0 && room >= pages * PAGE,
      `destination ${hex(destination)} is not free (${JSON.stringify(region)}). ` +
      'A stranded previous app image or another mapping (e.g. the exploit ' +
      'helper) already owns this base; rebuild the app at a different ' +
      '--code-base, or restart SmileBASIC if a previous launch leaked it.');
    /* The range command maps at most MAX_RANGE_PAGES per call. */
    for (let done = 0; done < pages; done += MAX_RANGE_PAGES) {
      const n = Math.min(MAX_RANGE_PAGES, pages - done);
      await this.mapRange({source: source + done * PAGE, destination: destination + done * PAGE,
        pages: n, permission});
    }
    const record = {source, destination, pages, permission, bytes};
    this.mapped.push(record);
    return record;
  }

  /* Cache maintenance: the bulk map finalises permissions but does not
     publish new code.  One patch per 64-byte line flushes and invalidates the
     whole line, so a 64-byte stride covers the image. */
  async flushCode(destination, image) {
    /* The range map's permission change publishes the new code: a launcher
     * mapped without any per-line publish ran 60 frames correctly on hardware
     * (2026-10-28), and mapping dropped from ~94 s to ~0.25 s.  Set
     * HBL_PUBLISH=1 to force the old per-line flush (e.g. after rewriting code
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
    /* The bulk unmap command validates the source range against the source
     * block's reported size and returned 0xD401 for an app image whose source
     * was exactly page-rounded.  The single-page command has no such check, so
     * unmap page by page (cheap: a handful of pages, one round trip each) and
     * fall back to the bulk command if the per-page form is rejected. */
    /* Large images: one bulk command (sources carry a slack page, which the
     * bulk command needs), per-page as the fallback. */
    if (pages > 64) {
      try {
        for (let done = 0; done < pages; done += MAX_RANGE_PAGES) {
          const n = Math.min(MAX_RANGE_PAGES, pages - done);
          await this.unmapRange({source: entry.source + done * PAGE,
            destination: entry.destination + done * PAGE, pages: n});
        }
        this.mapped = this.mapped.filter(item =>
          !(item.destination === entry.destination && item.source === entry.source));
        return;
      } catch (error) {
        this.log?.warn('bulk unmap failed, unmapping page by page: ' + error.message);
      }
    }
    /* Page-by-page takes ~30 ms a page: for big images that is minutes of
     * blocked host.  Leave them mapped (the caller then keeps the source)
     * and report it; relink at another base or restart SmileBASIC. */
    if (pages > 2048) {
      throw new Error(`bulk unmap failed; leaving ${pages} pages at ${hex(entry.destination)} mapped`);
    }
    let lastError = 0;
    for (let i = 0; i < pages; i++) {
      const reply = await this.client.request(CMD_UNMAP_PAGE, HANDLE,
        [this.pid, entry.source + i * PAGE, entry.destination + i * PAGE, PAGE]);
      if (Number(reply.result)) { lastError = Number(reply.result); break; }
    }
    if (lastError) {
      try {
        for (let done = 0; done < pages; done += MAX_RANGE_PAGES) {
          const n = Math.min(MAX_RANGE_PAGES, pages - done);
          await this.unmapRange({source: entry.source + done * PAGE,
            destination: entry.destination + done * PAGE, pages: n});
        }
        lastError = 0;
      } catch (error) {
        throw new Error(`unmap ${hex(entry.destination)} failed ` +
          `(per-page result ${hex(lastError)}): ${error.message}`);
      }
    }
    /* Drop the same object that mapImage stored. */
    this.mapped = this.mapped.filter(item =>
      !(item.destination === entry.destination && item.source === entry.source));
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
  async mapImage({source, destination, bytes, permission}) {
    const pages = Math.ceil(bytes / PAGE);
    const sourceRegion = this.regionOf(source, bytes);
    check(sourceRegion, 'mock map source outside a region');
    const copy = Buffer.from(sourceRegion.data.subarray(source - sourceRegion.address,
      source - sourceRegion.address + pages * PAGE));
    this.regions.push({address: destination, size: pages * PAGE, type: 0,
      permissions: permission, data: copy, mapped: true});
    this.mappedRanges.push({source, destination, pages, permission});
    return {pages};
  }
  async unmapImage(entry) {
    const index = this.regions.findIndex(region => region.address === entry.destination && region.mapped);
    if (index >= 0) this.regions.splice(index, 1);
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

module.exports = {LiveTarget, MockTarget, PAGE, HANDLE, MAX_RANGE_PAGES};
