'use strict';
/*
 * mock-device.cjs - a scripted stand-in for the launcher and app images.
 *
 * Used by tests and by `hbl-start --mock`, so the whole host lifecycle can run
 * with no console: the "launcher" returns a launch request, the "app" returns
 * a status, the launcher resumes, then asks to exit.
 */
const {ABI} = require('./runtime.cjs');

function createMockCallHandler(options = {}) {
  const state = {launcherCalls: 0, appCalls: 0, entries: []};
  const plan = options.plan || [
    {launch: 1},       /* pick the first title */
    {launch: 0},       /* resume, keep running */
    {launch: 0xffffffff}
  ];

  const handler = async (address, args, target) => {
    const ctxAddress = args[0];
    const ctx = await target.read(ctxAddress, ABI.ctxSize);
    const control = Number(ctx.readBigUInt64LE(ABI.ctx.control));
    const flags = ctx.readUInt32LE(ABI.ctx.flags);
    const isLauncher = state.launcherEntry === undefined || address === state.launcherEntry;
    if (address === state.appEntry) {
      state.appCalls++;
      /* Apps run a few frames, then report a clean exit. */
      const status = Buffer.alloc(8);
      status.writeBigUInt64LE(0n);
      await target.write(ctxAddress + ABI.ctx.status, status);
      const frames = Buffer.alloc(8);
      frames.writeBigUInt64LE(BigInt(options.appFrames || 60));
      await target.write(ctxAddress + ABI.ctx.frame, frames);
      const exit = Buffer.alloc(8);
      exit.writeBigUInt64LE(1n);
      await target.write(ctxAddress + ABI.ctx.exit, exit);
      return {result: 0};
    }
    if (isLauncher) {
      state.launcherCalls++;
      const step = plan[Math.min(state.launcherCalls - 1, plan.length - 1)];
      const word = Buffer.alloc(4);
      word.writeUInt32LE(step.launch >>> 0);
      await target.write(control + 4, word);
      state.entries.push({call: state.launcherCalls, resume: !!(flags & 1),
        launch: step.launch});
      const status = Buffer.alloc(8);
      status.writeBigUInt64LE(0n);
      await target.write(ctxAddress + ABI.ctx.status, status);
      /* Simulate the launcher publishing its renderer/GPU handles. */
      const gpu = Buffer.alloc(ABI.gpuSize);
      gpu.writeBigUInt64LE(BigInt(options.device || 0xabc000), ABI.gpu.device);
      gpu.writeBigUInt64LE(BigInt(options.queue || 0xabc100), ABI.gpu.queue);
      gpu.writeUInt32LE(2, ABI.gpu.image_count);
      for (let i = 0; i < 2; i++)
        gpu.writeBigUInt64LE(BigInt(0xabc200 + i), ABI.gpu.views + i * 8);
      await target.write(ctxAddress + ABI.ctx.gpu, gpu);
      const ui = Buffer.alloc(8);
      ui.writeBigUInt64LE(BigInt(options.ui || 0xabc900));
      await target.write(ctxAddress + ABI.ctx.ui, ui);
      return {result: 0};
    }
    return {result: 0};
  };

  handler.state = state;
  handler.setEntries = (launcherEntry, appEntry) => {
    state.launcherEntry = launcherEntry;
    state.appEntry = appEntry;
  };
  return handler;
}

module.exports = {createMockCallHandler};
