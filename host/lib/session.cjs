'use strict';
/*
 * session.cjs - acquire and verify the 4/4 console session.
 *
 * The exploit chain itself is not part of this repository: it lives in the
 * owner's ns2jb-kurumuz checkout, addressed through HBL_NS2JB (default
 * ../ns2jb-kurumuz).  This module only starts the runner, waits for a run
 * directory, verifies the stage reached 4/4, and exposes the run directory the
 * loader needs.
 *
 * Stage 4 is explicit and is never simulated: without "4/4 verified" there is
 * no native code path and the launcher refuses to map anything.
 */
const fs = require('node:fs');
const path = require('node:path');
const {spawn} = require('node:child_process');
const {check, readIfJson, sleep, HostError} = require('./common.cjs');

function ns2jbDir(override) {
  return path.resolve(override || process.env.HBL_NS2JB ||
    path.join(path.dirname(path.resolve(__dirname, '..', '..')), 'ns2jb-kurumuz'));
}

function runDirectories(directory) {
  const runs = path.join(directory, 'runs');
  if (!fs.existsSync(runs)) return [];
  return fs.readdirSync(runs).map(name => {
    const dir = path.join(runs, name);
    let stat;
    try { stat = fs.statSync(dir); } catch { return null; }
    if (!stat.isDirectory()) return null;
    return {dir, mtime: stat.mtimeMs, session: name};
  }).filter(Boolean).sort((a, b) => b.mtime - a.mtime);
}

function newestRun(directory) {
  const [first] = runDirectories(directory);
  return first ? first.dir : null;
}

async function probe(runDir, timeoutMs = 2500) {
  const result = {ok: false, session: null, stage: null, error: null};
  try {
    const rpc = require(path.join(runDir, 'shared/rpc.cjs'));
    result.session = String(rpc.a.session);
    const reply = await Promise.race([
      rpc.request({command: 'status'}),
      new Promise((_, reject) => setTimeout(() => reject(new Error('timeout')), timeoutMs))
    ]);
    result.ok = !!reply.ready;
    result.stage = reply.stage || null;
  } catch (error) {
    result.error = error.message;
  }
  return result;
}

/* Read the stage record the runner writes.  Returns the highest stage number
 * that is marked done, or 0. */
function stageOf(runDir) {
  const candidates = ['stage.json', 'health.json', 'status.json', 'run.json'];
  for (const name of candidates) {
    const record = readIfJson(path.join(runDir, name));
    if (!record) continue;
    if (typeof record.stage === 'number') return record.stage;
    if (typeof record.verified === 'string') return record.verified === '4/4 verified' ? 4 : 0;
  }
  const log = path.join(runDir, 'run.log');
  if (fs.existsSync(log)) {
    const text = fs.readFileSync(log, 'utf8');
    if (text.includes('4/4 verified')) return 4;
    const stages = [...text.matchAll(/(\d)\/4/g)].map(match => Number(match[1]));
    return stages.length ? Math.max(...stages) : 0;
  }
  return 0;
}

async function waitForVerified(runDir, options = {}) {
  const deadline = Date.now() + (options.timeoutMs || 120000);
  while (Date.now() < deadline) {
    const stage = stageOf(runDir);
    if (stage >= 4) return stage;
    if (options.log && options.log.stage) {
      /* quiet polling; the runner prints its own progress */
    }
    await sleep(1000);
  }
  return stageOf(runDir);
}

async function spawnRunner(options) {
  const directory = ns2jbDir(options.ns2jbDir);
  const runner = path.join(directory, 'run.cjs');
  check(fs.existsSync(runner), `runner not found: ${runner} (set HBL_NS2JB)`, 'session');
  const logFile = options.logFile || '/tmp/hbl-runner.log';
  const stream = fs.createWriteStream(logFile, {flags: 'w'});
  const child = spawn(process.execPath, ['run.cjs', '--rpc', options.rpc || '10.42.0.1'], {
    cwd: directory,
    stdio: ['ignore', 'pipe', 'pipe'],
    env: Object.assign({}, process.env, {NS2_RPC_TIMEOUT_MS: '86400000'})
  });
  child.stdout.pipe(stream);
  child.stderr.pipe(stream);
  options.log?.info('runner started', {pid: child.pid, log: logFile});
  return child;
}

/* The exact SmileBASIC steps (mirrors pico-pro-controller/RUNBOOK.md).  The
 * launcher prints them and can drive them through the Pico when --guide is
 * given. */
const GUIDE = [
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'dismiss a crash dialog'},
  {action: 'tap', button: 'HOME', delay: 1000, why: 'go to HOME'},
  {action: 'tap', button: 'X', ms: 250, delay: 1000, why: 'open Close the software'},
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'confirm close'},
  {action: 'tap', button: 'A', ms: 80, delay: 15000, why: 'launch SmileBASIC'},
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'dismiss information'},
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'dismiss DNS error'},
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'dismiss tag error'},
  {action: 'tap', button: 'A', ms: 80, delay: 4000, why: 'TOPMENU: Make a Program'},
  {action: 'text', text: 'load "txt:ns2"\n', delay: 3000, why: 'LOAD the bootstrap'},
  {action: 'tap', button: 'A', ms: 80, delay: 3000, why: 'confirm Load'},
  {action: 'text', text: 'run\n', delay: 8000, why: 'RUN the bootstrap'},
  {action: 'text', text: 'remove nsr,&h60000000,&h3fffffff\n', delay: 4000,
    why: 'free the NSR region'},
  {action: 'text', text: 'v=start(39380,10,42,0,1)\n', delay: 8000, why: 'start RPC'}
];

async function guide(options) {
  const script = options.picoScript ||
    process.env.HBL_PICO_TOOL ||
    path.join(path.dirname(path.resolve(__dirname, '..', '..')), 'pico-pro-controller/tools/pico_ctl.py');
  check(fs.existsSync(script), 'pico control tool not found; set HBL_PICO_TOOL', 'session');
  const run = (args) => new Promise((resolve, reject) => {
    const child = spawn('python3', [script, ...args], {stdio: 'inherit'});
    child.on('exit', code => code === 0 ? resolve() : reject(new Error('pico_ctl ' + args.join(' ') + ' exited ' + code)));
    child.on('error', reject);
  });
  for (const step of GUIDE) {
    options.log?.stage('guide', {action: step.action, why: step.why});
    if (step.action === 'tap')
      await run(['tap', step.button, ...(step.ms ? ['--ms', String(step.ms)] : [])]);
    else
      await run(['text', step.text]);
    await sleep(step.delay || 1000);
  }
  options.log?.info('SmileBASIC bootstrap sequence sent');
}

function printGuide(log) {
  log.info('SmileBASIC 4/4 recovery sequence (also in docs/RECOVERY.md):');
  for (const step of GUIDE) {
    const text = step.action === 'tap' ? `tap ${step.button}` : JSON.stringify(step.text);
    log.info(`  ${text.padEnd(44)} # ${step.why}`);
  }
}

module.exports = {ns2jbDir, runDirectories, newestRun, probe, stageOf, waitForVerified,
  spawnRunner, guide, printGuide, GUIDE};
