#!/usr/bin/env node
'use strict';
/*
 * hbl-start - one command from a cold console to the launcher.
 *
 *   hbl-start                 preflight, attach to (or start) a 4/4 session,
 *                             map the launcher and run it
 *   hbl-start --run DIR       use a specific run directory
 *   hbl-start --mock          run the whole lifecycle against the mock backend
 *   hbl-start --preflight     only run the checks
 *   hbl-start --guide         drive the SmileBASIC steps through the Pico
 *   hbl-start --spawn         start the runner and wait for 4/4 (default 300 s)
 *   hbl-start --wait N        how long --spawn waits for the session
 *   hbl-start --entries N     launcher entries before stopping (default 64)
 *   hbl-start --frames N      frames per launcher entry (default 720)
 *   hbl-start --seconds N     stop after N seconds
 *   hbl-start --teardown      free the session blocks instead of retaining them
 *   hbl-start --build         run tools/build-all.sh first
 *
 * Exit codes: 0 clean, 1 blocked (preflight/session), 2 usage, 3 runtime error.
 */
const fs = require('node:fs');
const path = require('node:path');

/* The runner and the run directory's rpc.cjs snapshot this when they load.
   A launcher entry can legitimately run for minutes, so default to 24 h. */
if (!process.env.NS2_RPC_TIMEOUT_MS) process.env.NS2_RPC_TIMEOUT_MS = '86400000';
const {Log, HostError, check, sleep} = require('./lib/common.cjs');
const preflight = require('./lib/preflight.cjs');
const session = require('./lib/session.cjs');
const {Runtime} = require('./lib/runtime.cjs');
const {LiveTarget, MockTarget} = require('./lib/target.cjs');
const {createMockCallHandler} = require('./lib/mock-device.cjs');

const ROOT = path.resolve(__dirname, '..');

function parseArgs(argv) {
  const options = {mock: false, preflight: false, guide: false, attach: true,
    seconds: 0, maxEntries: 64, maxFrames: 720, verbose: false};
  for (let i = 0; i < argv.length; i++) {
    const arg = argv[i];
    if (arg === '--mock') options.mock = true;
    else if (arg === '--preflight' || arg === '--check') options.preflight = true;
    else if (arg === '--guide') options.guide = true;
    else if (arg === '--attach') options.attach = true;
    else if (arg === '--spawn') options.attach = false;
    else if (arg === '--verbose' || arg === '-v') options.verbose = true;
    else if (arg === '--teardown') options.teardown = true;
    else if (arg === '--build') options.build = true;
    else if (arg === '--run') options.run = argv[++i];
    else if (arg === '--ns2jb') options.ns2jbDir = argv[++i];
    else if (arg === '--apps') options.appsDir = argv[++i];
    else if (arg === '--seconds') options.seconds = Number(argv[++i]);
    else if (arg === '--entries') options.maxEntries = Number(argv[++i]);
    else if (arg === '--frames') options.maxFrames = Number(argv[++i]);
    else if (arg === '--wait') options.wait = Number(argv[++i]);
    else if (arg === '--stage') options.stage = Number(argv[++i]);
    else if (arg === '--app') options.appId = argv[++i];
    else if (arg === '--app-stage') options.appStage = Number(argv[++i]);
    else if (arg === '--help' || arg === '-h') {
      console.log(fs.readFileSync(__filename, 'utf8').split('*/')[0]);
      process.exit(0);
    } else {
      console.error('hbl-start: unknown option ' + arg);
      process.exit(2);
    }
  }
  return options;
}

async function main() {
  const options = parseArgs(process.argv.slice(2));
  const logFile = options.mock
    ? path.join(ROOT, 'build/hbl-mock.log')
    : path.join(ROOT, 'build/hbl-session.log');
  const log = new Log({file: logFile, quiet: false});
  log.info('hbl-start', {version: '1.0.0', mock: options.mock});

  const checks = await preflight.run({ns2jbDir: options.ns2jbDir, mock: options.mock});
  const summary = preflight.summarise(checks, log);
  if (options.preflight) {
    console.log(JSON.stringify({ok: summary.ok, checks}, null, 2));
    return summary.ok ? 0 : 1;
  }
  if (!summary.ok) {
    log.error('preflight failed; fix the required checks above');
    return 1;
  }

  if (options.build) {
    log.stage('build');
    require('node:child_process').execFileSync('sh', [path.join(ROOT, 'tools/build-all.sh')],
      {stdio: 'inherit'});
  }

  let target;
  let runDir = options.run ? path.resolve(options.run) : null;
  if (options.mock) {
    const handler = createMockCallHandler();
    target = new MockTarget({log, onCall: handler});
    const runtime = new Runtime({target, log, appsDir: options.appsDir});
    await runtime.open();
    const firstNative = runtime.packages.find(pkg => pkg.manifest.kind === 'native' && pkg.report);
    if (!firstNative) {
      log.error('mock run needs at least one built native package (sh tools/build-apps.sh)');
      return 1;
    }
    handler.setEntries(runtime.launcher.entry, Number(BigInt(firstNative.report.entry)));
    const result = await runtime.run({maxEntries: options.maxEntries, maxFrames: options.maxFrames});
    await runtime.stop({teardown: options.teardown});
    console.log(JSON.stringify({mode: 'mock', result,
      launcherCalls: handler.state.launcherCalls, appCalls: handler.state.appCalls}, null, 2));
    return 0;
  }

  const directory = session.ns2jbDir(options.ns2jbDir);
  if (!runDir) {
    const newest = session.newestRun(directory);
    const stage = newest ? session.stageOf(newest) : 0;
    if (newest && stage >= 4) runDir = newest;
  }
  let runner = null;
  if (!runDir) {
    if (options.attach) {
      session.printGuide(log);
      log.error('no 4/4 session yet.  Follow docs/RECOVERY.md, then re-run hbl-start, ' +
        'or use --spawn to start the runner and wait.');
      return 1;
    }
    runner = await session.spawnRunner({ns2jbDir: options.ns2jbDir, log});
    if (options.guide) await session.guide({log});
    else session.printGuide(log);
    /* One command: wait for the session to reach 4/4 once the runner is up. */
    const waitMs = (options.wait || 300) * 1000;
    const deadline = Date.now() + waitMs;
    log.info(`waiting up to ${Math.round(waitMs / 1000)} s for 4/4 verified`);
    let lastStage = -1;
    while (Date.now() < deadline) {
      const candidate = session.newestRun(directory);
      if (candidate) {
        const stage = session.stageOf(candidate);
        if (stage !== lastStage) {
          log.stage('session stage', {run: path.basename(candidate), stage});
          lastStage = stage;
        }
        if (stage >= 4) { runDir = candidate; break; }
      }
      await sleep(3000);
    }
    if (!runDir) {
      log.error('no 4/4 session yet; the runner is still up.  Follow docs/RECOVERY.md ' +
        'and re-run hbl-start.');
      return 1;
    }
  }
  const stage = await session.waitForVerified(runDir, {timeoutMs: 5000, log});
  if (stage < 4) {
    log.error(`run ${path.basename(runDir)} is at stage ${stage}/4; stage 4 is required`);
    return 1;
  }

  target = await LiveTarget.open({runDir, log});
  const runtime = new Runtime({target, runDir, log, appsDir: options.appsDir});
  try {
    await runtime.open();
    const deadline = options.seconds > 0 ? Date.now() + options.seconds * 1000 : 0;
    const result = options.appId
      ? {app: options.appId, status: await runtime.launchOneApp(options)}
      : await runtime.run({maxEntries: options.maxEntries,
          maxFrames: options.maxFrames, deadline, stage: options.stage || 0});
    log.stage('session summary', result);
    return 0;
  } catch (error) {
    log.error(error.message, {stack: error.stack});
    return 3;
  } finally {
    await runtime.stop({teardown: options.teardown});
  }
}

main().then(code => process.exit(code)).catch(error => {
  console.error('hbl-start: ' + (error.stack || error.message));
  process.exit(error instanceof HostError ? 1 : 3);
});
