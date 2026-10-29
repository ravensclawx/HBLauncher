'use strict';
/*
 * compat.cjs - NES and SM64 compatibility titles.
 *
 * These titles are not SDK apps: their proven loaders already know how to
 * relocate the SM64 module, patch its imports and drive the NES core.  Rather
 * than reimplementing that machine under time pressure, the launcher delegates
 * to the proven loader for one finite session and then repaints itself.
 *
 * Contract (documented in docs/COMPAT.md):
 *   - the loader runs in its own process against the same console session;
 *   - it runs a *finite* number of frames and cleans up its own mappings;
 *   - the launcher's image and display stay mapped, so the return path is just
 *     another launcher entry with HBX_FLAG_RESUME;
 *   - audio is stripped (--no-av for SM64; the NES package sets audio off).
 *
 * Paths are configuration, never hard-coded: HBL_COMPAT_ROOT or the manifest
 * "driver" fields point at the loader and its arguments.
 */
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {check, HostError} = require('./common.cjs');

const STATUS_MISSING = 7;   /* HBX_ERR_UNSUPPORTED */
const STATUS_FAILED = 6;

function resolveDriver(runtime, pkg) {
  const driver = pkg.manifest.driver || {};
  const root = process.env.HBL_COMPAT_ROOT || driver.root || '';
  const candidates = [];
  if (driver.command) candidates.push(driver.command);
  if (pkg.manifest.kind === 'sm64') {
    candidates.push(path.join(root, 'sm64-console/host/map-sm64.cjs'));
    candidates.push(path.join(root, 'host/map-sm64.cjs'));
  } else if (pkg.manifest.kind === 'nes') {
    candidates.push(path.join(root, 'nes-port/host/run-nes.cjs'));
    candidates.push(path.join(root, 'host/run-nes.cjs'));
  }
  return {
    script: candidates.find(file => file && fs.existsSync(file)) || null,
    args: driver.args || [],
    frames: driver.frames || (pkg.manifest.kind === 'sm64' ? 1800 : 1800)
  };
}

function runChild(script, args, {timeoutMs, log}) {
  return new Promise((resolve, reject) => {
    const child = spawn(process.execPath, [script, ...args], {
      stdio: ['ignore', 'pipe', 'pipe'],
      env: Object.assign({}, process.env, {NS2_RPC_TIMEOUT_MS: process.env.NS2_RPC_TIMEOUT_MS || '86400000'})
    });
    const output = [];
    child.stdout.on('data', chunk => output.push(chunk));
    child.stderr.on('data', chunk => output.push(chunk));
    const timer = setTimeout(() => {
      child.kill('SIGKILL');
      reject(new HostError('compatibility loader timed out', 'compat-timeout'));
    }, timeoutMs);
    child.on('error', error => { clearTimeout(timer); reject(error); });
    child.on('exit', code => {
      clearTimeout(timer);
      const text = Buffer.concat(output).toString('utf8');
      log.info('compatibility loader finished', {code, tail: text.slice(-1500)});
      resolve({code, text});
    });
  });
}

async function launch(runtime, pkg) {
  const driver = resolveDriver(runtime, pkg);
  const kind = pkg.manifest.kind;
  if (!driver.script) {
    runtime.log.error(`${kind} package ${pkg.manifest.id} has no proven loader configured`);
    return STATUS_MISSING;
  }
  if (runtime.target.kind !== 'live') {
    runtime.log.info('mock session: compatibility launch simulated', {id: pkg.manifest.id});
    return 0;
  }
  const runDir = runtime.runDir;
  check(runDir, 'compatibility titles need a live run directory');
  const args = driver.args.length
    ? driver.args.map(value => String(value).replace('{run}', runDir))
    : [runDir, ...(kind === 'sm64'
        ? ['--present', '--frames', String(driver.frames), '--no-av']
        : [String(driver.frames), '1'])];
  runtime.log.stage('compat launch', {kind, script: driver.script, args});
  try {
    const result = await runChild(driver.script, args, {
      timeoutMs: driver.timeoutMs || 15 * 60 * 1000,
      log: runtime.log
    });
    return result.code === 0 ? 0 : STATUS_FAILED;
  } catch (error) {
    runtime.log.error('compatibility loader failed', {error: error.message});
    return STATUS_FAILED;
  }
}

module.exports = {launch, resolveDriver};
