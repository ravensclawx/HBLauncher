#!/usr/bin/env node
'use strict';
/*
 * host/probe.cjs - map and run the native probe without a display.
 *
 * Usage: node host/probe.cjs <run-dir> [--keep]
 *
 * Prints a JSON report and exits non-zero unless the probe returned 0x5A and
 * wrote its magic marker.  This is the first thing to run on hardware: it
 * exercises the live loader (bulk RX map + cache publish), the context ABI and
 * the entry trampoline without any Vulkan involvement.
 */
const fs = require('node:fs');
const path = require('node:path');
const {Log, check, hex} = require('./lib/common.cjs');
const {LiveTarget, MockTarget} = require('./lib/target.cjs');
const {Ctx, ABI} = require('./lib/runtime.cjs');

const ROOT = path.resolve(__dirname, '..');
const CODE_BASE = 0x09000000;
const MAGIC = 0x5A5A1234;

/* A scripted stand-in for the probe image, so the host path can be tested
   without a console (tests/run.sh runs this). */
function mockHandler() {
  return async (address, args, target) => {
    const ctx = args[0];
    const bytes = await target.read(ctx, ABI.ctxSize);
    const logStruct = Number(bytes.readBigUInt64LE(ABI.ctx.log));
    const descriptor = await target.read(logStruct, 16);
    const ring = Number(descriptor.readBigUInt64LE(0));
    const text = 'probe: entry\nprobe: ok\n';
    await target.write(ring, Buffer.from(text));
    const magic = Buffer.alloc(8);
    magic.writeBigUInt64LE(BigInt(MAGIC));
    await target.write(ctx + ABI.ctx.frame, magic);
    const reached = Buffer.alloc(8);
    reached.writeBigUInt64LE(5n);
    await target.write(ctx + ABI.ctx.args + 16, reached);
    return {result: 0x5a};
  };
}

async function main() {
  const args = process.argv.slice(2);
  if (args.includes('--help') || args.includes('-h') || args.length === 0) {
    console.log('usage: node host/probe.cjs <run-dir> [--step N] [--keep] | --mock');
    return 0;
  }
  const stepIndex = args.indexOf('--step');
  const step = stepIndex >= 0 ? Number(args[stepIndex + 1]) : 5;
  const mock = args.includes('--mock');
  const runDir = args[0] && !args[0].startsWith('-') ? path.resolve(args[0]) : null;
  const keep = args.includes('--keep');
  check(mock || runDir, 'usage: node host/probe.cjs <run-dir> [--keep] | --mock');
  const log = new Log({file: path.join(ROOT, 'build/hbl-probe.log')});
  const target = mock ? new MockTarget({log, onCall: mockHandler()})
                      : await LiveTarget.open({runDir, log});
  const {report, code} = readReport();
  let codeSource = 0, ctxBlock = 0, heap = 0, stack = 0, logRing = 0, logStruct = 0;
  try {
    codeSource = await target.allocate(Math.ceil(code.length / 0x1000) * 0x1000);
    await target.write(codeSource, code);
    await target.mapImage({source: codeSource, destination: CODE_BASE,
      bytes: code.length, permission: 5});
    const flush = await target.flushCode(CODE_BASE, code);
    log.info('probe mapped', {pages: Math.ceil(code.length / 0x1000),
      publishes: flush.calls});

    heap = await target.allocate(0x40000);
    stack = await target.allocate(0x40000);
    ctxBlock = await target.allocate(0x1000);
    logRing = await target.allocate(0x4000);
    logStruct = await target.allocate(0x1000);
    await target.write(logRing, Buffer.alloc(0x4000));
    {
      const descriptor = Buffer.alloc(16);
      descriptor.writeBigUInt64LE(BigInt(logRing), 0);
      descriptor.writeUInt32LE(0x4000, 8);
      await target.write(logStruct, descriptor);
    }
    const ctx = Ctx.header()
      .u64('now_ns', 0n).u64('tick_hz', 19200000).u64('uptime_ns', 0n)
      .u64('heap_base', heap).u64('heap_size', 0x40000)
      .u64('code_base', CODE_BASE).u64('code_size', code.length)
      /* Step 0 stays on the caller's stack to isolate the entry trampoline. */
      .u64('stack_base', step >= 1 ? stack : 0)
      .u64('stack_size', step >= 1 ? 0x40000 : 0)
      .u64('log', logStruct)
      .u64('atlas', 0).u64('atlas_bytes', 0);
    ctx.buffer.writeBigUInt64LE(BigInt(step), ABI.ctx.args + 8);
    await target.write(ctxBlock, ctx.buffer);

    if (target.verify) await target.verify(Number(BigInt(report.entry)), code);
    const reply = await target.call(Number(BigInt(report.entry)), [ctxBlock]);
    const status = Number((await target.read(ctxBlock + ABI.ctx.status, 8)).readBigUInt64LE());
    const marker = Number((await target.read(ctxBlock + ABI.ctx.frame, 8)).readBigUInt64LE());
    const reached = Number((await target.read(ctxBlock + ABI.ctx.args + 16, 8)).readBigUInt64LE());
    const text = (await target.read(logRing, 0x4000)).toString('utf8').replace(/\0+$/, '');
    const ok = reply.result === 0x5a && marker === MAGIC && reached >= Math.min(step, 5);
    console.log(JSON.stringify({
      ok, step, reached, result: reply.result, status, marker: hex(marker), log: text.trim()
    }, null, 2));
    return ok ? 0 : 1;
  } finally {
    try {
      await target.unmapImage({source: codeSource, destination: CODE_BASE,
        bytes: code.length});
      if (!keep) for (const block of [codeSource, heap, stack, ctxBlock, logRing, logStruct]) {
        if (block) await target.free(block).catch(error => log.warn('free: ' + error.message));
      }
    } catch (error) {
      log.warn('cleanup: ' + error.message);
    }
    await target.close();
  }
}

function readReport() {
  const dir = path.join(ROOT, 'tests/probe/build');
  const reportFile = path.join(dir, 'report.json');
  const codeFile = path.join(dir, 'code.bin');
  check(fs.existsSync(reportFile) && fs.existsSync(codeFile),
    'probe is not built; run: python3 sdk/tools/hbx-build.py tests/probe');
  const report = JSON.parse(fs.readFileSync(reportFile, 'utf8'));
  const code = fs.readFileSync(codeFile);
  check(report.codeBytes === code.length, 'probe code.bin does not match its report');
  return {report, code};
}

main().then(code => process.exit(code)).catch(error => {
  console.error('probe: ' + (error.stack || error.message));
  process.exit(2);
});
