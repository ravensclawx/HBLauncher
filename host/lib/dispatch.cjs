'use strict';
/*
 * dispatch.cjs - build `struct hbx_vk_dispatch` for the current process.
 *
 * The header is the single source of truth for field order.  Every entry is
 * resolved from the SmileBASIC compatibility module's exported symbols:
 *
 *     address = module.base + symbol.offset
 *
 * module.base is ASLR-dependent, so the table is rebuilt for every session and
 * never cached across runs.
 */
const fs = require('node:fs');
const path = require('node:path');
const {check, sha256} = require('./common.cjs');

const ROOT = path.resolve(__dirname, '..', '..');
const DEFAULT_HEADER = path.join(ROOT, 'sdk/include/hbx_vk.h');

/* Entrypoints the launcher can live without; everything else is required. */
const OPTIONAL = new Set([
  'vkEnumerateInstanceExtensionProperties',
  'vkDestroySurfaceKHR',
  'vkDestroyInstance',
  'vkDestroyDevice',
  'vkDestroySwapchainKHR',
  'vkDestroyImageView',
  'vkDestroySemaphore',
  'vkDestroyImage',
  'vkDestroySampler',
  'vkDestroyDescriptorSetLayout',
  'vkDestroyDescriptorPool',
  'vkDestroyPipelineLayout',
  'vkDestroyShaderModule',
  'vkDestroyPipeline',
  'vkDestroyCommandPool',
  'vkGetPhysicalDeviceSurfacePresentModesKHR',
  'vkCmdCopyImageToBuffer',
  'vkAcquireNextImage2KHR',
  'vkGetPhysicalDeviceProperties'
]);

function parseDispatchHeader(headerPath = DEFAULT_HEADER) {
  const text = fs.readFileSync(headerPath, 'utf8');
  const start = text.indexOf('struct hbx_vk_dispatch {');
  check(start >= 0, 'hbx_vk_dispatch not found in ' + headerPath, 'dispatch');
  const end = text.indexOf('};', start);
  check(end > start, 'unterminated hbx_vk_dispatch', 'dispatch');
  const body = text.slice(start, end);
  const fields = [];
  const pattern = /PFN_(vk\w+)\s+(vk\w+)\s*;/g;
  let match;
  while ((match = pattern.exec(body))) {
    check(match[1] === match[2], `field ${match[2]} has type PFN_${match[1]}`, 'dispatch');
    fields.push(match[2]);
  }
  check(fields.length > 0, 'no PFN fields parsed', 'dispatch');
  check(new Set(fields).size === fields.length, 'duplicate dispatch field', 'dispatch');
  /* Cross-check against the compiled ABI so a header edit cannot silently
     change the table the loader builds. */
  const abiFile = path.join(ROOT, 'sdk/abi.json');
  if (fs.existsSync(abiFile)) {
    const abi = JSON.parse(fs.readFileSync(abiFile, 'utf8'));
    check(abi.vkDispatchFields === fields.length,
      `dispatch header has ${fields.length} fields but sdk/abi.json says ` +
      `${abi.vkDispatchFields}; run tools/gen_abi.py`, 'dispatch');
  }
  return fields;
}

function findExport(module, name) {
  return (module.symbols || []).find(s => s.name === name && s.section && s.type === 2) || null;
}

/* Returns {buffer, fields, missing, optionalMissing, base, manifest}. */
function buildDispatch(moduleJson, options = {}) {
  const module = typeof moduleJson === 'string' ? JSON.parse(moduleJson) : moduleJson;
  const base = BigInt(options.base !== undefined ? options.base : module.base);
  check(base > 0n, 'module base must be positive', 'dispatch');
  const fields = options.fields || parseDispatchHeader(options.header);
  const buffer = Buffer.alloc(fields.length * 8);
  const missing = [];
  const optionalMissing = [];
  const resolved = [];
  fields.forEach((field, index) => {
    const symbol = findExport(module, field);
    if (!symbol) {
      (OPTIONAL.has(field) ? optionalMissing : missing).push(field);
      return;
    }
    const address = base + BigInt(symbol.offset);
    buffer.writeBigUInt64LE(address, index * 8);
    resolved.push({field, offset: symbol.offset, address});
  });
  check(missing.length === 0,
    'dispatch table is missing required entrypoints: ' + missing.join(', '), 'dispatch');
  return {
    buffer,
    fields,
    resolved,
    missing,
    optionalMissing,
    base,
    size: buffer.length,
    sha256: sha256(buffer),
    manifest: {
      format: 'hblauncher-native-dispatch/1',
      base: '0x' + base.toString(16),
      fields: fields.length,
      bytes: buffer.length,
      resolved: resolved.length,
      optionalMissing,
      bufferSha256: sha256(buffer)
    }
  };
}

module.exports = {parseDispatchHeader, buildDispatch, DEFAULT_HEADER, OPTIONAL};
