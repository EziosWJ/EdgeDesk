#!/usr/bin/env node
/**
 * EdgeDesk extension packaging CLI.
 *
 * Commands:
 *   edgedesk-ext validate <dir>   Validate manifest and static assets
 *   edgedesk-ext pack <dir> [out] Validate and create a deterministic ZIP
 *   edgedesk-ext inspect <zip>    List package entries and validate manifest
 *
 * No external dependencies. Node.js is a development build tool only; the
 * device never runs Node.js for installed applications.
 */

import { promises as fs } from 'node:fs';
import path from 'node:path';
import { deflateRawSync } from 'node:zlib';
import { crc32 as zlibCrc32 } from 'node:zlib';
import { fileURLToPath } from 'node:url';

const HOST_API_VERSION = '1.0.0';
const PERMISSIONS = new Set(['storage', 'files.manage', 'terminal.access', 'network']);
const SEMVER = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$/;
const ID = /^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?(?:\.[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?)+$/;

// SPEC-0011 section 4.2. The SDK mirrors the host limits so a package fails
// during development instead of after upload. Identity, socket path, data
// directory and autostart are host-managed and never appear in the manifest.
const BACKEND_KEYS = new Set(['executable', 'args']);
const BACKEND_EXECUTABLE_MAX_BYTES = 4096;
const BACKEND_ARGS_MAX_ENTRIES = 128;
const BACKEND_ARG_MAX_BYTES = 8192;
const BACKEND_ARGS_TOTAL_MAX_BYTES = 128 * 1024;
const TEMPLATES_ROOT = fileURLToPath(new URL('../templates/', import.meta.url));
const ELF_MACHINES = new Map([
  [62, { arch: 'x86_64', elfClass: 2 }],
  [183, { arch: 'aarch64', elfClass: 2 }],
  [40, { arch: 'arm32', elfClass: 1 }],
]);

function fail(message) {
  const error = new Error(message);
  error.userFacing = true;
  throw error;
}

function controlChars(text) {
  return /[\u0000-\u001f\u007f]/.test(text);
}

function parseOrigin(text) {
  if (typeof text !== 'string' || text.length > 512) fail(`invalid network origin: ${text}`);
  const match = /^(https?):\/\/(\*\.)?(\[[0-9a-fA-F:]+\]|[A-Za-z0-9.-]+)(?::(\d{1,5}))?$/.exec(text);
  if (!match) fail(`invalid network origin (scheme://host[:port] required): ${text}`);
  if (!match[3].startsWith('[') && !/^[a-z0-9.-]+$/i.test(match[3])) fail(`invalid origin host: ${text}`);
  if (match[4] && (Number(match[4]) < 1 || Number(match[4]) > 65535)) fail(`invalid origin port: ${text}`);
  return text;
}

function normalizeEntry(name) {
  const normalized = name.replace(/\\/g, '/');
  if (!normalized || normalized.startsWith('/') || normalized.includes('\0')) fail(`invalid relative path: ${name}`);
  const parts = normalized.split('/');
  if (parts.some((part) => part === '' || part === '.' || part === '..')) fail(`invalid relative path: ${name}`);
  return normalized;
}

function byteLength(text) {
  return Buffer.byteLength(text, 'utf8');
}

function validateBackendExecutable(value, errors) {
  if (typeof value !== 'string' || value.length === 0) {
    errors.push('backend.executable is required and must be a non-empty string');
    return null;
  }
  if (byteLength(value) > BACKEND_EXECUTABLE_MAX_BYTES) {
    errors.push(`backend.executable must be at most ${BACKEND_EXECUTABLE_MAX_BYTES} bytes`);
    return null;
  }
  if (controlChars(value)) {
    errors.push('backend.executable must not contain NUL or control characters');
    return null;
  }
  if (value.includes('\\')) {
    errors.push('backend.executable must use forward slashes and no backslashes');
    return null;
  }
  if (value.startsWith('/')) {
    errors.push('backend.executable must be a relative path inside the package');
    return null;
  }
  const parts = value.split('/');
  if (parts.some((part) => part === '' || part === '.' || part === '..')) {
    errors.push('backend.executable must be a normalized relative path (no empty, "." or ".." segments)');
    return null;
  }
  return value;
}

function validateBackendArgs(value, errors) {
  if (value === undefined) return [];
  if (!Array.isArray(value)) {
    errors.push('backend.args must be an array of strings');
    return [];
  }
  if (value.length > BACKEND_ARGS_MAX_ENTRIES) {
    errors.push(`backend.args must contain at most ${BACKEND_ARGS_MAX_ENTRIES} entries`);
  }
  let total = 0;
  value.forEach((argument, index) => {
    if (typeof argument !== 'string') {
      errors.push(`backend.args[${index}] must be a string`);
      return;
    }
    if (controlChars(argument)) {
      errors.push(`backend.args[${index}] must not contain NUL or control characters`);
      return;
    }
    const size = byteLength(argument);
    total += size;
    if (size > BACKEND_ARG_MAX_BYTES) {
      errors.push(`backend.args[${index}] must be at most ${BACKEND_ARG_MAX_BYTES} bytes`);
    }
  });
  if (total > BACKEND_ARGS_TOTAL_MAX_BYTES) {
    errors.push(`backend.args must total at most ${BACKEND_ARGS_TOTAL_MAX_BYTES} bytes`);
  }
  return value;
}

function validateBackend(value, errors) {
  if (typeof value !== 'object' || value === null || Array.isArray(value)) {
    errors.push('backend must be an object');
    return null;
  }
  for (const key of Object.keys(value)) {
    if (!BACKEND_KEYS.has(key)) {
      errors.push(
        `backend.${key} is not allowed; run identity, socket, data directory, autostart and network settings are managed by EdgeDesk`,
      );
    }
  }
  const executable = validateBackendExecutable(value.executable, errors);
  const args = validateBackendArgs(value.args, errors);
  return { executable, args };
}

function localElfArch() {
  const arch = { x64: 'x86_64', arm64: 'aarch64', arm: 'arm32' }[process.arch];
  return arch ?? 'unknown';
}

/**
 * Header-only ELF inspection. It never maps or executes package content and
 * never follows symlinks. Full layout validation happens on the device at
 * install time.
 */
async function inspectElfFile(file) {
  const handle = await fs.open(file, 'r');
  try {
    const header = Buffer.alloc(64);
    const { bytesRead } = await handle.read(header, 0, header.length, 0);
    if (bytesRead < 4 || !(header[0] === 0x7f && header[1] === 0x45 && header[2] === 0x4c && header[3] === 0x46)) {
      return { error: 'not an ELF file (scripts and other formats are not executed)' };
    }
    if (bytesRead < 52) return { error: 'file is too short to be an ELF executable' };
    const elfClass = header[4];
    if (elfClass !== 1 && elfClass !== 2) return { error: 'unsupported ELF class' };
    if (header[5] !== 1) return { error: 'big-endian ELF is not supported' };
    if (header[6] !== 1) return { error: 'unsupported ELF header version' };
    if (bytesRead < (elfClass === 2 ? 64 : 52)) return { error: 'truncated ELF header' };
    const type = header.readUInt16LE(16);
    if (type !== 2 && type !== 3) {
      return { error: 'ELF type must be an executable (ET_EXEC) or position-independent executable (ET_DYN)' };
    }
    const machine = header.readUInt16LE(18);
    const expected = ELF_MACHINES.get(machine);
    if (!expected) {
      return { error: `unsupported ELF architecture (e_machine=${machine}, class=${elfClass === 2 ? 64 : 32}-bit)` };
    }
    if (expected.elfClass !== elfClass) {
      return { error: `ELF class ${elfClass === 2 ? 64 : 32}-bit does not match ${expected.arch}` };
    }
    return { arch: expected.arch };
  } finally {
    await handle.close();
  }
}

function validateManifest(manifest) {
  const errors = [];
  const warnings = [];
  if (typeof manifest !== 'object' || manifest === null || Array.isArray(manifest)) {
    fail('manifest.json must contain a JSON object');
  }
  if (typeof manifest.id !== 'string' || !ID.test(manifest.id)) {
    errors.push('id must be a lowercase reverse-domain application id');
  }
  if (typeof manifest.name !== 'string' || !manifest.name.trim() || manifest.name.length > 120 || controlChars(manifest.name)) {
    errors.push('name must be 1..120 printable characters');
  }
  if (typeof manifest.version !== 'string' || !SEMVER.test(manifest.version)) {
    errors.push('version must be strict SemVer (major.minor.patch)');
  }
  if (manifest.entry !== 'index.html') errors.push("entry must be 'index.html'");
  if (typeof manifest.minHostApiVersion !== 'string' || !SEMVER.test(manifest.minHostApiVersion)) {
    errors.push('minHostApiVersion is required and must be SemVer');
  }
  if (typeof manifest.maxHostApiVersion !== 'string' || !SEMVER.test(manifest.maxHostApiVersion)) {
    errors.push('maxHostApiVersion is required and must be SemVer');
  }
  if (Array.isArray(manifest.permissions)) {
    for (const permission of manifest.permissions) {
      if (!PERMISSIONS.has(permission)) errors.push(`unknown permission module: ${permission}`);
    }
  } else if (manifest.permissions !== undefined) {
    errors.push('permissions must be an array');
  }
  const origins = manifest.network?.origins;
  if (origins !== undefined) {
    if (!Array.isArray(origins)) {
      errors.push('network.origins must be an array');
    } else {
      for (const origin of origins) parseOrigin(origin);
    }
  }
  if (manifest.window !== undefined) {
    if (typeof manifest.window !== 'object' || manifest.window === null) {
      errors.push('window must be an object');
    } else {
      const fields = ['defaultWidth', 'defaultHeight', 'minWidth', 'minHeight'];
      for (const field of fields) {
        const value = manifest.window[field];
        if (value !== undefined && (!Number.isInteger(value) || value < 1 || value > 10000)) {
          errors.push(`window.${field} must be an integer between 1 and 10000`);
        }
      }
      if (manifest.window.instances !== undefined && !['single', 'multi'].includes(manifest.window.instances)) {
        errors.push("window.instances must be 'single' or 'multi'");
      }
      const defaultWidth = manifest.window.defaultWidth ?? 960;
      const defaultHeight = manifest.window.defaultHeight ?? 640;
      if ((manifest.window.minWidth ?? 320) > defaultWidth) errors.push('window.minWidth exceeds defaultWidth');
      if ((manifest.window.minHeight ?? 240) > defaultHeight) errors.push('window.minHeight exceeds defaultHeight');
    }
  }
  if (manifest.icon !== undefined && (typeof manifest.icon !== 'string' || !/^[^/\\][^\\]*$/.test(manifest.icon) || manifest.icon.split('/').some((part) => part === '..' || part === '.'))) {
    errors.push('icon must be a normalized relative path');
  }
  let backend = null;
  if (manifest.backend !== undefined) {
    backend = validateBackend(manifest.backend, errors);
  }
  if (manifest.runtime !== undefined || manifest.scripts !== undefined) {
    warnings.push('runtime/script fields are reserved and are not executed by EdgeDesk');
  }
  if (errors.length > 0) fail(`manifest validation failed:\n- ${errors.join('\n- ')}`);
  if (backend) manifest.backend = backend;
  return { manifest, warnings, hostCompatible: currentHostInRange(manifest) };
}

function currentHostInRange(manifest) {
  const parse = (text) => text.split(/[+-]/)[0].split('.').map(Number);
  const compare = (a, b) => {
    for (let index = 0; index < 3; index += 1) {
      if (a[index] !== b[index]) return a[index] < b[index] ? -1 : 1;
    }
    return 0;
  };
  const host = parse(HOST_API_VERSION);
  return compare(parse(manifest.minHostApiVersion), host) <= 0 && compare(host, parse(manifest.maxHostApiVersion)) <= 0;
}

async function walk(root) {
  const files = [];
  const directories = [];
  const seen = new Map();
  async function visit(absolute, relative) {
    const entries = await fs.readdir(absolute, { withFileTypes: true });
    entries.sort((left, right) => left.name.localeCompare(right.name, 'en'));
    for (const entry of entries) {
      const childAbsolute = path.join(absolute, entry.name);
      const childRelative = relative ? `${relative}/${entry.name}` : entry.name;
      if (entry.isSymbolicLink()) fail(`symlinks are not allowed in packages: ${childRelative}`);
      if (entry.isDirectory()) {
        directories.push(childRelative);
        await visit(childAbsolute, childRelative);
      } else if (entry.isFile()) {
        const normalized = normalizeEntry(childRelative);
        const folded = normalized.toLowerCase();
        if (seen.has(folded)) fail(`duplicate package path (case-insensitive): ${normalized} and ${seen.get(folded)}`);
        seen.set(folded, normalized);
        files.push(normalized);
      } else {
        fail(`special files are not allowed: ${childRelative}`);
      }
    }
  }
  await visit(root, '');
  return { files, directories };
}

async function readManifest(root) {
  const file = path.join(root, 'manifest.json');
  let text;
  try {
    text = await fs.readFile(file, 'utf8');
  } catch {
    fail('manifest.json is missing');
  }
  let parsed;
  try {
    parsed = JSON.parse(text);
  } catch (error) {
    fail(`manifest.json is not valid JSON: ${error.message}`);
  }
  return validateManifest(parsed);
}

// Kept synchronous-friendly by reading only small text assets.
let awaitTextCache = new Map();

async function loadTextAssets(root, files) {
  awaitTextCache = new Map();
  for (const file of files) {
    if (/\.(html?|css|js|mjs)$/i.test(file)) {
      const info = await fs.stat(path.join(root, file));
      if (info.size <= 2 * 1024 * 1024) awaitTextCache.set(file, await fs.readFile(path.join(root, file), 'utf8'));
    }
  }
}

async function validateDirectory(root) {
  const stat = await fs.stat(root).catch(() => null);
  if (!stat || !stat.isDirectory()) fail(`not a directory: ${root}`);
  const { manifest, warnings } = await readManifest(root);
  const { files, directories } = await walk(root);
  const folded = new Set(files.map((file) => file.toLowerCase()));
  if (!folded.has('index.html')) fail('index.html is required at the package root');
  if (!folded.has('manifest.json')) fail('manifest.json is required at the package root');
  if (manifest.icon && !folded.has(manifest.icon.toLowerCase())) fail(`manifest icon does not exist: ${manifest.icon}`);
  if (manifest.backend) {
    const executable = manifest.backend.executable;
    if (!files.includes(executable)) fail(`backend.executable does not exist in the package: ${executable}`);
    const stat = await fs.lstat(path.join(root, executable));
    if (!stat.isFile()) fail(`backend.executable must be a regular file: ${executable}`);
    if ((stat.mode & 0o111) === 0) {
      fail(`backend.executable is not executable: ${executable} (set the executable bit before packing)`);
    }
    const elf = await inspectElfFile(path.join(root, executable));
    if (elf.error) fail(`backend.executable is not a valid ELF executable: ${elf.error}`);
    if (elf.arch !== localElfArch()) {
      warnings.push(
        `backend.executable targets ${elf.arch}; this validator host is ${localElfArch()}. ` +
          'The device independently checks the ELF architecture at install time.',
      );
    }
  }
  await loadTextAssets(root, files);
  const referenceWarnings = [];
  const referenceErrors = [];
  const fileSet = new Set(files);
  for (const [file, text] of awaitTextCache) {
    if (/\.(html?)$/i.test(file)) {
      const matches = [...text.matchAll(/\b(?:src|href)\s*=\s*["']([^"']+)["']/gi)];
      for (const match of matches) {
        const target = match[1].trim();
        if (!target || target.startsWith('#') || /^(?:[a-z]+:|data:|\/\/)/i.test(target)) continue;
        if (target.startsWith('/')) {
          referenceErrors.push(`${file} uses an absolute path (${target}); use a relative path`);
          continue;
        }
        const resolved = path.posix.normalize(path.posix.join(path.posix.dirname(file), target.replace(/[?#].*$/, '')));
        if (resolved.startsWith('../') || resolved === '..') {
          referenceErrors.push(`${file} references outside the package: ${target}`);
        } else if (!fileSet.has(resolved) && !resolved.endsWith('/')) {
          referenceWarnings.push(`${file} references missing asset: ${target}`);
        }
      }
    }
    if (/\.css$/i.test(file)) {
      for (const match of text.matchAll(/url\(\s*["']?([^"')]+)["']?\s*\)/gi)) {
        const target = match[1].trim();
        if (/^(?:[a-z]+:|data:|\/\/)/i.test(target)) continue;
        if (target.startsWith('/')) referenceErrors.push(`${file} uses an absolute CSS url (${target})`);
      }
    }
  }
  if (referenceErrors.length > 0) fail(`static asset validation failed:\n- ${referenceErrors.join('\n- ')}`);
  return {
    manifest,
    warnings: [...warnings, ...referenceWarnings],
    files,
    directories,
    root,
  };
}

const CRC_TABLE = (() => {
  const table = new Uint32Array(256);
  for (let index = 0; index < 256; index += 1) {
    let value = index;
    for (let bit = 0; bit < 8; bit += 1) value = value & 1 ? (value >>> 1) ^ 0xedb88320 : value >>> 1;
    table[index] = value >>> 0;
  }
  return table;
})();

function crc32(buffer) {
  if (typeof zlibCrc32 === 'function') return zlibCrc32(buffer) >>> 0;
  let crc = 0xffffffff;
  for (const byte of buffer) crc = (crc >>> 8) ^ CRC_TABLE[(crc ^ byte) & 0xff];
  return (crc ^ 0xffffffff) >>> 0;
}

function zipEntryName(name) {
  return Buffer.from(name, 'utf8');
}

function buildZip(entries) {
  if (entries.length > 65535) fail('ZIP entry count exceeds the classic ZIP limit');
  const chunks = [];
  const central = [];
  let offset = 0;
  for (const entry of entries) {
    const name = zipEntryName(entry.name);
    const compressed = deflateRawSync(entry.data, { level: 9 });
    if (entry.data.length > 0xffffffff || compressed.length > 0xffffffff || offset > 0xffffffff) {
      fail('package exceeds the classic ZIP size limit');
    }
    const crc = crc32(entry.data);
    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);
    local.writeUInt16LE(0x0800, 6);
    local.writeUInt16LE(8, 8);
    local.writeUInt16LE(0, 10);
    local.writeUInt16LE(0, 12);
    local.writeUInt32LE(crc, 14);
    local.writeUInt32LE(compressed.length, 18);
    local.writeUInt32LE(entry.data.length, 22);
    local.writeUInt16LE(name.length, 26);
    local.writeUInt16LE(0, 28);
    chunks.push(local, name, compressed);
    central.push({ name, crc, compressed: compressed.length, uncompressed: entry.data.length, offset, mode: entry.mode });
    offset += local.length + name.length + compressed.length;
  }
  const centralOffset = offset;
  for (const entry of central) {
    const header = Buffer.alloc(46);
    header.writeUInt32LE(0x02014b50, 0);
    header.writeUInt16LE(0x031e, 4);
    header.writeUInt16LE(20, 6);
    header.writeUInt16LE(0x0800, 8);
    header.writeUInt16LE(8, 10);
    header.writeUInt16LE(0, 12);
    header.writeUInt16LE(0, 14);
    header.writeUInt32LE(entry.crc, 16);
    header.writeUInt32LE(entry.compressed, 20);
    header.writeUInt32LE(entry.uncompressed, 24);
    header.writeUInt16LE(entry.name.length, 28);
    header.writeUInt16LE(0, 30);
    header.writeUInt16LE(0, 32);
    header.writeUInt16LE(0, 34);
    header.writeUInt16LE(0, 36);
    header.writeUInt32LE(((0o100000 | ((entry.mode ?? 0o644) & 0o777)) << 16) >>> 0, 38);
    header.writeUInt32LE(entry.offset, 42);
    chunks.push(header, entry.name);
    offset += header.length + entry.name.length;
  }
  const centralSize = offset - centralOffset;
  const eocd = Buffer.alloc(22);
  eocd.writeUInt32LE(0x06054b50, 0);
  eocd.writeUInt16LE(0, 4);
  eocd.writeUInt16LE(0, 6);
  eocd.writeUInt16LE(central.length, 8);
  eocd.writeUInt16LE(central.length, 10);
  eocd.writeUInt32LE(centralSize, 12);
  eocd.writeUInt32LE(centralOffset, 16);
  eocd.writeUInt16LE(0, 20);
  chunks.push(eocd);
  return Buffer.concat(chunks);
}

async function packDirectory(root, output) {
  const validation = await validateDirectory(root);
  const entries = [];
  for (const relative of validation.files) {
    const absolute = path.join(root, relative);
    const stat = await fs.stat(absolute);
    entries.push({ name: relative, data: await fs.readFile(absolute), mode: stat.mode });
  }
  const zip = buildZip(entries);
  await fs.writeFile(output, zip);
  return { ...validation, output, bytes: zip.length };
}

async function readZipEntries(file) {
  const data = await fs.readFile(file);
  let eocd = -1;
  for (let index = data.length - 22; index >= 0; index -= 1) {
    if (data.readUInt32LE(index) === 0x06054b50) {
      eocd = index;
      break;
    }
  }
  if (eocd < 0) fail('not a ZIP archive');
  const count = data.readUInt16LE(eocd + 10);
  let offset = data.readUInt32LE(eocd + 16);
  const entries = [];
  for (let index = 0; index < count; index += 1) {
    if (offset + 46 > data.length || data.readUInt32LE(offset) !== 0x02014b50) fail('invalid ZIP central directory');
    const nameLength = data.readUInt16LE(offset + 28);
    const extraLength = data.readUInt16LE(offset + 30);
    const commentLength = data.readUInt16LE(offset + 32);
    const name = data.subarray(offset + 46, offset + 46 + nameLength).toString('utf8');
    entries.push({
      name,
      compressed: data.readUInt32LE(offset + 20),
      uncompressed: data.readUInt32LE(offset + 24),
      localOffset: data.readUInt32LE(offset + 42),
      mode: data.readUInt32LE(offset + 38) >>> 16,
    });
    offset += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

async function initTemplate(destination, templateName) {
  if (!/^[a-z0-9][a-z0-9-]*$/.test(templateName)) fail(`invalid template name: ${templateName}`);
  const source = path.join(TEMPLATES_ROOT, templateName);
  const stat = await fs.stat(source).catch(() => null);
  if (!stat || !stat.isDirectory()) {
    const available = await fs.readdir(TEMPLATES_ROOT).catch(() => []);
    fail(`unknown template: ${templateName} (available: ${available.join(', ')})`);
  }
  const existing = await fs.readdir(destination).catch(() => null);
  if (existing !== null && existing.length > 0) fail(`destination is not empty: ${destination}`);
  await fs.mkdir(destination, { recursive: true });
  await fs.cp(source, destination, { recursive: true, force: true, errorOnExist: false });
  return {
    destination,
    template: templateName,
    files: await readdirRecursive(destination),
  };
}

async function readdirRecursive(root, relative = '') {
  const entries = await fs.readdir(root, { withFileTypes: true });
  const files = [];
  for (const entry of entries) {
    const child = relative ? `${relative}/${entry.name}` : entry.name;
    if (entry.isDirectory()) files.push(...(await readdirRecursive(path.join(root, entry.name), child)));
    else files.push(child);
  }
  return files;
}

function usage() {
  return `EdgeDesk extension CLI\n\nUsage:\n  edgedesk-ext validate <dir>\n  edgedesk-ext pack <dir> [output.zip]\n  edgedesk-ext inspect <package.zip>\n  edgedesk-ext init <dir> [--template <name>]\n\nTemplates: vanilla, react, vue, svelte, native-backend.\n\nThe directory must contain manifest.json and index.html at its root and a\nbuilt static application. Host API ${HOST_API_VERSION}. Native backend\npackages also need backend.executable staged as a native ELF file.\n`;
}

async function main(argv) {
  const [command, ...args] = argv;
  if (!command || command === '--help' || command === '-h') {
    process.stdout.write(usage());
    return 0;
  }
  if (command === 'validate') {
    if (!args[0]) fail('validate requires a directory');
    const result = await validateDirectory(path.resolve(args[0]));
    process.stdout.write(
      `valid: ${result.manifest.id} ${result.manifest.version} (${result.files.length} files)\n` +
        (result.warnings.length ? `warnings:\n- ${result.warnings.join('\n- ')}\n` : ''),
    );
    return 0;
  }
  if (command === 'pack') {
    if (!args[0]) fail('pack requires a directory');
    const root = path.resolve(args[0]);
    const output = path.resolve(args[1] ?? `${path.basename(root)}.edgedesk.zip`);
    const result = await packDirectory(root, output);
    process.stdout.write(
      `packed: ${result.output} (${result.manifest.id} ${result.manifest.version}, ${result.bytes} bytes)\n` +
        (result.warnings.length ? `warnings:\n- ${result.warnings.join('\n- ')}\n` : ''),
    );
    return 0;
  }
  if (command === 'inspect') {
    if (!args[0]) fail('inspect requires a ZIP file');
    const entries = await readZipEntries(path.resolve(args[0]));
    const names = entries.map((entry) => entry.name);
    if (!names.includes('manifest.json')) fail('package does not contain manifest.json');
    if (!names.includes('index.html')) fail('package does not contain index.html');
    const listing = entries
      .map((entry) => `  ${entry.name}${(entry.mode & 0o111) !== 0 ? ' [exec]' : ''}`)
      .join('\n');
    process.stdout.write(`${entries.length} entries:\n${listing}\n`);
    return 0;
  }
  if (command === 'init') {
    if (!args[0]) fail('init requires a destination directory');
    let template = 'vanilla';
    for (let index = 1; index < args.length; index += 1) {
      if (args[index] === '--template') {
        template = args[index + 1];
        if (!template) fail('--template requires a name');
        index += 1;
      } else {
        fail(`unknown init argument: ${args[index]}`);
      }
    }
    const result = await initTemplate(path.resolve(args[0]), template);
    process.stdout.write(
      `created: ${result.destination} from template ${result.template} (${result.files.length} files)\n`,
    );
    if (template === 'native-backend') {
      process.stdout.write(
        'next: build backend/notes-backend with make, then run validate/pack.\n',
      );
    }
    return 0;
  }
  process.stderr.write(usage());
  return 2;
}

main(process.argv.slice(2))
  .then((code) => process.exit(code))
  .catch((error) => {
    process.stderr.write(`${error.userFacing ? 'error: ' : 'unexpected error: '}${error.message}\n`);
    process.exit(1);
  });
