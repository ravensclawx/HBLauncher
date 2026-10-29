'use strict';
/* Shared host helpers: logging, assertions, hex, hashing, timing. */
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');

class HostError extends Error {
  constructor(message, code) {
    super(message);
    this.name = 'HostError';
    this.code = code || 'host-error';
  }
}

const check = (ok, message, code) => {
  if (!ok) throw new HostError(message, code);
};

const hex = value => '0x' + BigInt(value).toString(16);
const sha256 = buffer => crypto.createHash('sha256').update(buffer).digest('hex');
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (error) {
    throw new HostError(`cannot read ${file}: ${error.message}`, 'io');
  }
}

function readIfJson(file, fallback = null) {
  try { return readJson(file); } catch { return fallback; }
}

function ensureDir(dir) {
  fs.mkdirSync(dir, { recursive: true });
  return dir;
}

/* Structured, timestamped log lines.  The launcher session writes the same
   stream to the console and to <run>/hbl-session.log. */
class Log {
  constructor(options = {}) {
    this.file = options.file || null;
    this.quiet = !!options.quiet;
    this.entries = [];
    if (this.file) ensureDir(path.dirname(this.file));
  }
  line(level, message, extra) {
    const entry = {at: new Date().toISOString(), level, message};
    if (extra) entry.data = extra;
    this.entries.push(entry);
    if (!this.quiet || level === 'error' || level === 'warn') {
      const suffix = extra ? ' ' + JSON.stringify(extra) : '';
      const stream = level === 'error' || level === 'warn' ? process.stderr : process.stdout;
      stream.write(`[hbl ${level}] ${message}${suffix}\n`);
    }
    if (this.file) fs.appendFileSync(this.file, JSON.stringify(entry) + '\n');
    return entry;
  }
  info(message, extra) { return this.line('info', message, extra); }
  warn(message, extra) { return this.line('warn', message, extra); }
  error(message, extra) { return this.line('error', message, extra); }
  stage(name, extra) { return this.line('stage', name, extra); }
}

module.exports = {HostError, check, hex, sha256, sleep, readJson, readIfJson, ensureDir, Log};
