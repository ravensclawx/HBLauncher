'use strict';
/*
 * preflight.cjs - prove the environment before touching the console.
 *
 * Every check returns {name, ok, required, detail, fix}.  Nothing here maps
 * memory or calls the exploit; it only reads local artifacts and probes the
 * network/controller endpoints.
 */
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const {execFileSync} = require('node:child_process');
const {sha256, readIfJson} = require('./common.cjs');
const session = require('./session.cjs');

const ROOT = path.resolve(__dirname, '..', '..');

function tcpProbe(host, port, timeoutMs = 1200) {
  return new Promise(resolve => {
    const socket = net.connect({host, port});
    const done = ok => { socket.destroy(); resolve(ok); };
    socket.setTimeout(timeoutMs);
    socket.on('connect', () => done(true));
    socket.on('timeout', () => done(false));
    socket.on('error', () => done(false));
  });
}

async function run(options = {}) {
  const checks = [];
  /* The exploit chain is only needed for a live session; the mock backend
     exercises everything else without a console. */
  const live = !options.mock;
  const add = (name, ok, required, detail, fix) =>
    checks.push({name, ok, required, detail, fix});

  /* Toolchain */
  add('node', true, true, process.version, 'install Node 18+');
  for (const tool of ['aarch64-linux-gnu-gcc', 'aarch64-linux-gnu-objcopy', 'aarch64-linux-gnu-nm']) {
    let ok = true;
    try { execFileSync('sh', ['-c', `command -v ${tool}`], {stdio: 'ignore'}); }
    catch { ok = false; }
    add(tool, ok, true, ok ? 'found' : 'missing', 'apt install gcc-aarch64-linux-gnu');
  }

  /* Built artifacts */
  const launcherReport = path.join(ROOT, 'launcher/build/report.json');
  const launcherCode = path.join(ROOT, 'launcher/build/code.bin');
  let built = false;
  if (fs.existsSync(launcherReport) && fs.existsSync(launcherCode)) {
    try {
      const report = JSON.parse(fs.readFileSync(launcherReport, 'utf8'));
      built = report.codeSha256 === sha256(fs.readFileSync(launcherCode));
    } catch { built = false; }
  }
  add('launcher build', built, true, built ? 'code.bin matches report.json' : 'missing or stale',
    './launcher/build.sh');

  const atlas = path.join(ROOT, 'assets/font_atlas.r8');
  add('font atlas', fs.existsSync(atlas) && fs.statSync(atlas).size === 1024 * 1024, true,
    fs.existsSync(atlas) ? `${fs.statSync(atlas).size} bytes` : 'missing',
    'python3 tools/make_font_atlas.py');

  const abi = readIfJson(path.join(ROOT, 'sdk/abi.json'));
  add('sdk/abi.json', !!abi && abi.abiVersion === 3, true,
    abi ? `abi ${abi.abiVersion}, ctx ${abi.ctxSize} bytes` : 'missing', 'python3 tools/gen_abi.py');

  const header = path.join(ROOT, 'sdk/include/hbx_vk.h');
  add('dispatch header', fs.existsSync(header), true, header, 'restore sdk/include/hbx_vk.h');

  /* Apps */
  const appsDir = path.join(ROOT, 'apps');
  const apps = fs.existsSync(appsDir)
    ? fs.readdirSync(appsDir).filter(name => fs.existsSync(path.join(appsDir, name, 'manifest.json')))
    : [];
  add('app packages', apps.length > 0, false, apps.length ? apps.join(', ') : 'none built',
    'sh tools/build-apps.sh');

  /* Exploit chain (external to this repo) */
  const directory = session.ns2jbDir(options.ns2jbDir);
  const runner = path.join(directory, 'run.cjs');
  add('ns2jb runner', fs.existsSync(runner), live, directory,
    'set HBL_NS2JB to the ns2jb-kurumuz checkout');

  const expected = process.env.HBL_BULK_SHA256 ||
    'f661a1f190b8ddd0fc49d7c0c1592e3718f9b70efdb64a2bf5db2f0c9cb56a47';
  const payload = process.env.HBL_PAYLOAD ||
    path.join(directory, 'engine/switch2/ro/payload');
  let payloadOk = true;
  let payloadDetail = 'payload hash not pinned to a file (set HBL_PAYLOAD)';
  if (fs.existsSync(payload) && fs.statSync(payload).isFile()) {
    const digest = sha256(fs.readFileSync(payload));
    payloadOk = digest === expected;
    payloadDetail = `${digest.slice(0, 16)} (${fs.statSync(payload).size} bytes)`;
  }
  add('bulk payload hash', payloadOk, false, payloadDetail,
    `expected ${expected.slice(0, 16)}; rebuild with engine/build-payload.cjs`);

  /* Host-side network for the console link. */
  {
    let detail = 'no 10.42.0.x interface';
    let ok = false;
    try {
      const addresses = execFileSync('sh', ['-c',
        "ip -o -4 addr show 2>/dev/null | awk '{print $2, $4}'"], {encoding: 'utf8'});
      const line = addresses.split('\n').find(entry => entry.includes('10.42.0.'));
      if (line) {
        const device = line.trim().split(/\s+/)[0].replace(/:$/, '');
        let state = 'unknown';
        try {
          state = execFileSync('sh', ['-c', `ip -o link show ${device} | awk '{print $9}'`],
            {encoding: 'utf8'}).trim();
        } catch { state = 'unknown'; }
        ok = state === 'UP';
        detail = `${device} ${line.trim().split(/\s+/)[1]} state ${state}`;
      }
    } catch { /* iproute2 missing */ }
    add('console link address', ok, false, detail,
      'connect the wired adapter and run: sudo ip addr add 10.42.0.1/24 dev <iface>');
  }

  /* Pico web status page shows the chain stage. */
  {
    let detail = 'unreachable';
    let ok = false;
    try {
      const body = execFileSync('sh', ['-c',
        "curl -s -m 2 http://192.168.4.30:8099/ | head -c 200000"], {encoding: 'utf8'});
      const badge = /id="chainBadge"[^>]*>([^<]*)</.exec(body);
      const stage = /session <b id="chainSession">([^<]*)</.exec(body);
      ok = !!body;
      detail = ok ? `chain ${badge ? badge[1] : 'unknown'}` +
        (stage ? `, session ${stage[1]}` : '') : 'unreachable';
    } catch { /* curl missing or page down */ }
    add('pico web status', ok, false, detail,
      'open http://192.168.4.30:8099/ and check the exploit chain panel');
  }

  /* Live console reachability */
  const rpcHost = process.env.HBL_RPC_TARGET || '10.42.0.1';
  const rpcUp = await tcpProbe(rpcHost, 39380);
  add('console RPC', rpcUp, false, rpcUp ? `${rpcHost}:39380 accepting` : 'not listening',
    'run: node run.cjs --rpc ' + rpcHost + ' and start the SmileBASIC bootstrap');

  const newest = session.newestRun(directory);
  let stage = 0;
  let stageState = 'no run directory';
  if (newest) {
    const probe = await session.probe(newest);
    stage = session.stageOf(newest);
    stageState = `${path.basename(newest)} stage ${stage}/4` +
      (probe.ok ? ', RPC ready' : ', RPC not ready: ' + (probe.error || 'unknown'));
  }
  add('4/4 session', stage >= 4, false, stageState, 'follow docs/RECOVERY.md');

  /* The Pico is a UDP controller; the reliable liveness signal is its web
     page on 8099 (same device), which the check above already fetched. */
  {
    let picoUp = false;
    try {
      execFileSync('sh', ['-c',
        'curl -s -m 2 -o /dev/null -w "%{http_code}" http://192.168.4.30:8099/'],
        {encoding: 'utf8', stdio: 'pipe'});
      picoUp = true;
    } catch { picoUp = false; }
    add('pico controller', picoUp, false,
      picoUp ? 'web controller 192.168.4.30:8099' : 'unreachable',
      'power the Pico and join the Wi-Fi network');
  }

  add('capture device', fs.existsSync('/dev/video2'), false,
    fs.existsSync('/dev/video2') ? '/dev/video2' : 'missing',
    'attach the Elgato capture device');

  return checks;
}

function summarise(checks, log) {
  const requiredFailures = checks.filter(check => check.required && !check.ok);
  const optionalFailures = checks.filter(check => !check.required && !check.ok);
  for (const check of checks) {
    const level = check.ok ? 'info' : (check.required ? 'error' : 'warn');
    log[level](`preflight: ${check.name}: ${check.detail}`,
      check.ok ? undefined : {fix: check.fix});
  }
  return {ok: requiredFailures.length === 0, requiredFailures, optionalFailures};
}

module.exports = {run, summarise, tcpProbe};
