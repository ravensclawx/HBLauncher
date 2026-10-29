'use strict';
/*
 * inspect.cjs - ensure the per-run artifacts the loader needs exist.
 *
 * compatibility-module.json (the module's export table) is produced by the
 * archived display/compatibility inspectors and is per console session.  The
 * launcher used to fail without it; now it is generated on demand.
 */
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

const ROOT = path.resolve(__dirname, '..', '..');

function archiveDirectory() {
  const candidates = [
    process.env.HBL_ARCHIVE_SDK,
    path.join(path.dirname(ROOT), 'archive/hblauncher-cpu-sdk/homebrew-launcher'),
    path.join(ROOT, 'reference/hblauncher-cpu-sdk/homebrew-launcher')
  ].filter(Boolean);
  return candidates.find(dir => fs.existsSync(path.join(dir, 'inspect-compatibility.cjs'))) || null;
}

function ensureModuleArtifacts(runDir, log) {
  if (fs.existsSync(path.join(runDir, 'compatibility-module.json'))) return true;
  const dir = archiveDirectory();
  if (!dir) {
    log?.warn('compatibility-module.json missing and no inspector found ' +
      '(set HBL_ARCHIVE_SDK)');
    return false;
  }
  for (const script of ['inspect-display.cjs', 'inspect-compatibility.cjs']) {
    const file = path.join(dir, script);
    if (!fs.existsSync(file)) continue;
    log?.stage('inspect', {script});
    const result = spawnSync(process.execPath, [file, runDir], {encoding: 'utf8'});
    if (result.status !== 0) {
      log?.warn(`${script} failed: ${(result.stderr || result.stdout || '').slice(-300)}`);
    }
  }
  return fs.existsSync(path.join(runDir, 'compatibility-module.json'));
}

module.exports = {ensureModuleArtifacts, archiveDirectory};
