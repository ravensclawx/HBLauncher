'use strict';
/*
 * LiveTarget coverage without a console: a payload-faithful fake ns2jb client.
 *
 * The fake encodes what the audited ns2jb payload (kurumuz fork, commit
 * 81d5bb1, engine/switch2/ro/payload/process.c) actually implements:
 *   - worker-control allowlist: command IDs 0x524f0010-0x524f0016 only
 *   - 0x524f0012: one page per call, page-aligned Normal/RW source, free
 *     destination, size exactly 0x1000, destination finalised RX
 *   - 0x524f0013: one page per call, matching source/destination
 *
 * Everything LiveTarget sends goes through this fake, so the test fails if
 * HBLauncher ever speaks a command ID the published payload rejects, or if
 * the map/unmap arguments drift from what the payload validates.
 *
 * Run: node tests/host/live-target.test.cjs   (also part of tests/run.sh)
 */
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert');

const ROOT = path.resolve(__dirname, '..', '..');
const {LiveTarget, MockTarget, PAGE, HANDLE, SDK_ALLOCATE, SDK_FREE,
  CMD_MAP_PAGE, CMD_UNMAP_PAGE} = require(path.join(ROOT, 'host/lib/target.cjs'));
const {HostError} = require(path.join(ROOT, 'host/lib/common.cjs'));

const ALLOWED = [0x524f0010, 0x524f0011, 0x524f0012, 0x524f0013, 0x524f0014,
  0x524f0015, 0x524f0016];
const BAD_INPUT = 0xf601;

/* An in-process address space with the payload's own validation rules.
 * regions: [{address, size, type, permissions, data}] */
class FakeConsole {
  constructor(options = {}) {
    this.regions = [];
    this.calls = [];
    this.failMapAtPage = options.failMapAtPage ?? -1; /* page index that fails */
    this.mapCalls = 0;
    this.unmapCalls = 0;
  }
  regionOf(address, length = 1) {
    return this.regions.find(region =>
      address >= region.address && address + length <= region.address + region.size);
  }
  /* ns2jb payload process_request(), trimmed to the probes HBLauncher uses. */
  async request(id, handle, values) {
    this.calls.push({id, handle, values});
    if (!ALLOWED.includes(id)) throw new Error('Unknown process probe');
    assert.strictEqual(values.length, 4, 'process command input must be 32 bytes');
    assert.strictEqual(handle, HANDLE, 'process command must address the owned handle');
    const [pid, source, destination, size] = values.map(Number);
    assert.strictEqual(pid, this.pid, 'process command pid mismatch');
    if (id === CMD_MAP_PAGE) {
      this.mapCalls++;
      const page = (destination - this.nextDestination) / PAGE;
      if ((source | destination) & 0xfff || size !== 0x1000)
        return reply(BAD_INPUT);
      const src = this.query(source);
      if (src.type !== 5 || src.permissions !== 3 ||
          source + size > src.address + src.size)
        return reply(BAD_INPUT);
      if (this.failMapAtPage >= 0 && page >= this.failMapAtPage)
        return reply(0xd201);
      const dest = this.query(destination);
      if (dest.type !== 0 || destination + size > dest.address + dest.size)
        return reply(BAD_INPUT);
      const data = this.regionOf(source, PAGE).data.subarray(source - this.regionOf(source).address,
        source - this.regionOf(source).address + PAGE);
      this.regions.push({address: destination, size: PAGE, type: 5,
        permissions: 5, data: Buffer.from(data)});
      return reply(0); /* values[3] map=0, [4] permission=0, [5] rollback=0 */
    }
    if (id === CMD_UNMAP_PAGE) {
      this.unmapCalls++;
      if ((source | destination) & 0xfff || size !== 0x1000) return reply(BAD_INPUT);
      const index = this.regions.findIndex(region =>
        region.address === destination && region.permissions === 5);
      if (index < 0) return reply(BAD_INPUT);
      this.regions.splice(index, 1);
      return reply(0);
    }
    throw new Error('fake does not implement 0x' + id.toString(16));
  }
  query(address) {
    const region = this.regions.find(item =>
      address >= item.address && address < item.address + item.size);
    if (region) return {address: region.address, size: region.size,
      type: region.type, permissions: region.permissions};
    return {address, size: 0x10000000, type: 0, permissions: 0};
  }
}

function reply(result, extra = {}) {
  const values = [0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n, 0n].map(BigInt);
  values[0] = BigInt(4242);            /* out[0] = pid */
  values[3] = BigInt(extra.mapResult ?? 0);
  values[4] = BigInt(extra.permissionResult ?? 0);
  values[5] = BigInt(extra.rollbackResult ?? 0);
  return {result: BigInt(result), values};
}

/* Build a LiveTarget wired to the fake without going through open(), which
 * needs a real verified session.  rpc mirrors engine/shared/rpc.cjs shapes. */
function liveTarget(fake, options = {}) {
  const target = new LiveTarget({runDir: '/nonexistent', log: null});
  target.pid = 4242;
  fake.pid = 4242;
  fake.nextDestination = options.nextDestination ?? 0x08000000;
  const scratch = {block: null};
  target.rpc = {
    a: {session: 'fake-session', sdk: 0x7000000000, main: 0x8000000000},
    output: 0x2000f000,
    call: async (offset, args) => {
      if (offset === SDK_ALLOCATE) {
        const base = fake.regions.length ?
          fake.regions[fake.regions.length - 1].address +
          fake.regions[fake.regions.length - 1].size :
          0x40000000;
        const align = Number(args[1]), size = Number(args[2]);
        const at = Math.ceil(base / align) * align;
        fake.regions.push({address: at, size, type: 5, permissions: 3,
          data: Buffer.alloc(size)});
        scratch.block = Buffer.alloc(16);
        scratch.block.writeBigUInt64LE(BigInt(at), 0);
        return {result: 0n, output: scratch.block};
      }
      if (offset === SDK_FREE) {
        const address = Number(args[0]);
        const index = fake.regions.findIndex(region => region.address === address);
        if (index >= 0) fake.regions.splice(index, 1);
        return {result: 0n, output: Buffer.alloc(0)};
      }
      throw new Error('unexpected SDK call ' + offset);
    },
    read: async (address, length) => {
      const parts = [];
      let at = address, end = address + length;
      while (at < end) {
        const region = fake.regionOf(at);
        if (!region) throw new Error('fake read outside any region at 0x' + at.toString(16));
        const take = Math.min(end - at, region.address + region.size - at);
        parts.push(region.data.subarray(at - region.address, at - region.address + take));
        at += take;
      }
      return Buffer.concat(parts);
    },
    write: async (address, bytes) => {
      let at = address;
      const region = fake.regionOf(at, bytes.length);
      if (!region) throw new Error('fake write outside any region at 0x' + at.toString(16));
      /* native.cjs guard: only RW (permission 3) regions are writable -
         pages mapped through 0x524f0012 are RX and must fault on write. */
      if (region.permissions !== 3)
        throw new Error('fake write to a non-writable (permissions ' + region.permissions +
          ') region at 0x' + at.toString(16));
      bytes.copy(region.data, at - region.address);
    },
    query: async address => fake.query(address),
    request: async body => {
      if (body.command === 'verify-function') return {verified: true};
      throw new Error('unexpected rpc command ' + body.command);
    },
    callAddress: async (address, args) => ({result: 0x5an, output: Buffer.alloc(0)})
  };
  target.client = fake;
  target.freeAddress = 0x40000000;
  target.session = 'fake-session';
  return target;
}

const tests = [];
const test = (name, fn) => tests.push({name, fn});

/* Sentence 1 of the mission, kept executable: nothing in host/ may reference
 * the dead bulk command IDs, and the SDK offsets stay at the audited values. */
test('no dead command ids in host/, SDK offsets unchanged', () => {
  const walk = dir => fs.readdirSync(dir, {withFileTypes: true}).flatMap(entry => {
    const full = path.join(dir, entry.name);
    return entry.isDirectory() ? walk(full) : full;
  });
  const hits = [];
  for (const file of walk(path.join(ROOT, 'host'))) {
    if (!file.endsWith('.cjs')) continue;
    const text = fs.readFileSync(file, 'utf8');
    for (const id of ['0x524f001a', '0x524f001b', '0x524f001c'])
      if (text.includes(id)) hits.push(path.relative(ROOT, file) + ' references ' + id);
  }
  assert.deepStrictEqual(hits, [], 'host/ must not reference dead command ids: ' +
    JSON.stringify(hits));
  assert.strictEqual(SDK_ALLOCATE, 5732112, 'SDK_ALLOCATE must stay at the audited offset');
  /* operator delete(void*) -> free(); 5797280 is memset and leaked blocks. */
  assert.strictEqual(SDK_FREE, 5616480, 'SDK_FREE must stay at operator delete');
});

test('LiveTarget maps an image page by page and unmaps it', async () => {
  const fake = new FakeConsole();
  const target = liveTarget(fake, {nextDestination: 0x08000000});
  const source = await target.allocate(3 * PAGE); /* 3 pages > payload max */
  const image = Buffer.alloc(3 * PAGE - 17);
  for (let i = 0; i < image.length; i++) image[i] = i & 0xff;
  await target.write(source, image);
  await target.mapImage({source, destination: 0x08000000,
    bytes: image.length, permission: 5});
  assert.strictEqual(fake.mapCalls, 3, 'one 0x524f0012 call per page');
  const mapCalls = fake.calls.filter(call => call.id === CMD_MAP_PAGE);
  assert.strictEqual(mapCalls.length, 3);
  mapCalls.forEach((call, page) => {
    /* [pid, source, destination, PAGE] in payload input order. */
    assert.deepStrictEqual(call.values.map(Number), [4242,
      mapCalls[0].values[1] + page * PAGE, 0x08000000 + page * PAGE, PAGE],
      'map call carries [pid, source, destination, PAGE]');
  });
  /* The destination reads back the image and is RX per the payload. */
  assert((await target.read(0x08000000, image.length)).equals(image),
    'mapped bytes must match the staged image');
  const dest = fake.query(0x08000000);
  assert.strictEqual(dest.type, 5);
  assert.strictEqual(dest.permissions, 5, 'payload finalises the destination RX');
  await target.unmapImage({source, destination: 0x08000000, bytes: image.length});
  assert.strictEqual(fake.unmapCalls, 3, 'one 0x524f0013 call per page');
  assert.strictEqual(fake.query(0x08000000).type, 0, 'destination is free again');
  await target.free(source);
});

test('LiveTarget map failure rolls back earlier pages', async () => {
  const fake = new FakeConsole({failMapAtPage: 1}); /* page 1 of 3 fails */
  const target = liveTarget(fake, {nextDestination: 0x0B000000});
  const source = await target.allocate(3 * PAGE);
  const image = Buffer.alloc(3 * PAGE);
  await target.write(source, image);
  await assert.rejects(() => target.mapImage({source, destination: 0x0B000000,
    bytes: image.length, permission: 5}), HostError);
  assert.strictEqual(fake.mapCalls, 2, 'mapping stopped at the failing page');
  assert.strictEqual(fake.unmapCalls, 1, 'the page mapped before the failure was rolled back');
  assert.strictEqual(fake.query(0x0B000000).type, 0,
    'rollback leaves the destination free');
  assert.strictEqual(target.mapped.length, 0, 'no record survives a failed map');
  await target.free(source);
});

test('LiveTarget refuses a destination that is not free', async () => {
  const fake = new FakeConsole();
  const target = liveTarget(fake);
  fake.regions.push({address: 0x08000000, size: PAGE, type: 5,
    permissions: 5, data: Buffer.alloc(PAGE)});
  const source = await target.allocate(PAGE);
  await assert.rejects(() => target.mapImage({source, destination: 0x08000000,
    bytes: PAGE, permission: 5}), /not free/);
  assert.strictEqual(fake.mapCalls, 0, 'no map call is sent when the pre-check fails');
  await target.free(source);
});

test('mapImage refuses a writable mapping; data must go through placeData', async () => {
  const fake = new FakeConsole();
  const target = liveTarget(fake);
  const source = await target.allocate(PAGE);
  await target.write(source, Buffer.alloc(PAGE, 1));
  /* Asking the payload's only map command for RW would silently produce RX
     pages (process_permission 5 is hardcoded inside 0x524f0012), so the
     first .bss write would fault.  mapImage must refuse up front. */
  await assert.rejects(() => target.mapImage({source, destination: 0x08000000,
    bytes: PAGE, permission: 3}), /RX|placeData/);
  assert.strictEqual(fake.mapCalls, 0, 'no 0x524f0012 call may be sent for writable data');
  /* And the payload-faithful fake rejects writes into a mapped (RX) page. */
  await target.mapImage({source, destination: 0x08000000, bytes: PAGE, permission: 5});
  await assert.rejects(() => target.write(0x08000000, Buffer.alloc(16)),
    /non-writable/, 'an RX-mapped page must not be writable');
  await target.unmapImage({source, destination: 0x08000000, bytes: PAGE});
  await target.free(source);
});

test('placeData yields a writable RW block (not RX) that round-trips', async () => {
  const fake = new FakeConsole();
  const target = liveTarget(fake);
  const content = Buffer.alloc(300);
  for (let i = 0; i < content.length; i++) content[i] = (i * 13) & 0xff;
  const block = await target.placeData({bytes: 2 * PAGE, content});
  const info = fake.query(block);
  assert.strictEqual(Number(info.type), 5, 'data block is Normal memory');
  assert.strictEqual(Number(info.permissions), 3, 'data block is RW');
  assert.notStrictEqual(Number(info.permissions), 5,
    'the data region must never end up RX (the payload cannot map RW)');
  /* Written content is readable at offset 0. */
  assert((await target.read(block, content.length)).equals(content),
    'placeData content did not land');
  /* The image writes its runtime state well past the SDK persist header
     (offset 512..) and reads it back: the hardware RW gate, offline. */
  const pattern = Buffer.alloc(512);
  for (let i = 0; i < pattern.length; i++) pattern[i] = (i * 7 + 3) & 0xff;
  await target.write(block + 512, pattern);
  assert((await target.read(block + 512, pattern.length)).equals(pattern),
    'data region write/readback mismatch');
  const tail = Buffer.alloc(64, 0xa5);
  await target.write(block + 2 * PAGE - 64, tail);
  assert((await target.read(block + 2 * PAGE - 64, 64)).equals(tail),
    'data region tail write/readback mismatch');
  /* Zeroed outside the content: the image expects a zero-filled block. */
  assert((await target.read(block + 1024, 16)).equals(Buffer.alloc(16)),
    'placeData must zero the region beyond its content');
  await target.free(block);
});

test('LiveTarget sends only allowlisted command ids', async () => {
  const fake = new FakeConsole();
  const target = liveTarget(fake);
  const source = await target.allocate(2 * PAGE);
  await target.write(source, Buffer.alloc(2 * PAGE));
  await target.mapImage({source, destination: 0x08000000, bytes: 2 * PAGE, permission: 5});
  await target.unmapImage({source, destination: 0x08000000, bytes: 2 * PAGE});
  for (const call of fake.calls)
    assert(ALLOWED.includes(call.id), 'command id 0x' + call.id.toString(16) +
      ' is not in the ns2jb allowlist');
});

test('MockTarget map path enforces the payload source rules', async () => {
  const mock = new MockTarget({});
  const source = await mock.allocate(2 * PAGE);
  await mock.write(source, Buffer.alloc(2 * PAGE, 1));
  await mock.mapImage({source, destination: 0x08000000, bytes: 2 * PAGE, permission: 5});
  const region = mock.regionOf(0x08000000, PAGE);
  assert(region, 'destination region exists after map');
  assert.strictEqual(region.permissions, 5);
  assert((await mock.read(0x08000000, 16)).equals(Buffer.alloc(16, 1)));
  /* The fabricated free-destination write region is gone: writes must fail. */
  await assert.rejects(() => mock.write(0x0D000000, Buffer.alloc(16)),
    /outside any region/,
    'a free destination must not be writable (native.cjs guard)');
  await mock.unmapImage({source, destination: 0x08000000, bytes: 2 * PAGE});
  assert(!mock.regionOf(0x08000000, PAGE), 'destination freed by unmap');
});

(async () => {
  let failed = 0;
  for (const item of tests) {
    try {
      await item.fn();
      console.log(`ok   ${item.name}`);
    } catch (error) {
      failed++;
      console.error(`FAIL ${item.name}\n     ${error.message}`);
    }
  }
  console.log(`\n${tests.length - failed}/${tests.length} live-target tests passed`);
  process.exit(failed ? 1 : 0);
})();
