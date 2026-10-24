import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const here = path.dirname(fileURLToPath(import.meta.url));
const sdkRoot = path.join(here, '..');
const cli = path.join(sdkRoot, 'bin', 'edgedesk-ext.js');
const template = path.join(sdkRoot, 'templates', 'native-backend');

function run(args, cwd) {
  return new Promise((resolve) => {
    execFile(process.execPath, [cli, ...args], { cwd }, (error, stdout, stderr) => {
      resolve({ code: error ? error.code ?? 1 : 0, stdout, stderr });
    });
  });
}

async function tempDir() {
  return fs.mkdtemp(path.join(os.tmpdir(), 'edgedesk-backend-test-'));
}

/** A syntactically valid ELF header; the host performs full validation. */
function elfHeader({ machine = 62, elfClass = 2, type = 2, bigEndian = false } = {}) {
  const header = Buffer.alloc(64);
  header[0] = 0x7f;
  header[1] = 0x45;
  header[2] = 0x4c;
  header[3] = 0x46;
  header[4] = elfClass;
  header[5] = bigEndian ? 2 : 1;
  header[6] = 1;
  header.writeUInt16LE(type, 16);
  header.writeUInt16LE(machine, 18);
  return header;
}

async function makeApp(root, { backend, ...overrides } = {}) {
  const manifest = {
    id: 'io.example.backend',
    name: 'Backend Test',
    version: '1.0.0',
    entry: 'index.html',
    minHostApiVersion: '1.0.0',
    maxHostApiVersion: '1.9.9',
    permissions: [],
    ...overrides,
  };
  if (backend !== undefined) manifest.backend = backend;
  await fs.mkdir(root, { recursive: true });
  await fs.writeFile(path.join(root, 'manifest.json'), JSON.stringify(manifest));
  await fs.writeFile(path.join(root, 'index.html'), '<!doctype html><h1>backend</h1>');
  return manifest;
}

async function writeBackend(root, { contents, mode = 0o755, executable = 'backend/notes-backend' } = {}) {
  const file = path.join(root, executable);
  await fs.mkdir(path.dirname(file), { recursive: true });
  await fs.writeFile(file, contents ?? elfHeader());
  await fs.chmod(file, mode);
  return executable;
}

async function withApp(options, callback) {
  const root = await tempDir();
  try {
    await makeApp(root, options);
    return await callback(root);
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
}

test('valid backend manifest with args validates and packs with executable mode', async () => {
  await withApp({ backend: { executable: 'backend/notes-backend', args: ['--mode', 'notes'] } }, async (root) => {
    await writeBackend(root);
    const validated = await run(['validate', root]);
    assert.equal(validated.code, 0, validated.stderr);
    assert.match(validated.stdout, /valid: io\.example\.backend 1\.0\.0/);

    const output = path.join(root, 'app.zip');
    const packed = await run(['pack', root, output]);
    assert.equal(packed.code, 0, packed.stderr);

    const inspected = await run(['inspect', output]);
    assert.equal(inspected.code, 0, inspected.stderr);
    assert.match(inspected.stdout, /backend\/notes-backend \[exec\]/);
    assert.doesNotMatch(inspected.stdout, /index\.html \[exec\]/);
  });
});

test('backend without args and Web-only manifests stay valid', async () => {
  await withApp({ backend: { executable: 'backend/notes-backend' } }, async (root) => {
    await writeBackend(root);
    const result = await run(['validate', root]);
    assert.equal(result.code, 0, result.stderr);
  });

  await withApp({}, async (root) => {
    const result = await run(['validate', root]);
    assert.equal(result.code, 0, result.stderr);
    assert.doesNotMatch(result.stdout, /warning/);
  });
});

test('rejects unknown backend keys', async () => {
  const keys = ['run_user', 'run_group', 'root', 'allow_root', 'autostart', 'socket', 'data_dir', 'url', 'port'];
  for (const key of keys) {
    await withApp({ backend: { executable: 'backend/notes-backend', [key]: 'x' } }, async (root) => {
      await writeBackend(root);
      const result = await run(['validate', root]);
      assert.notEqual(result.code, 0, `expected rejection for backend.${key}`);
      assert.match(result.stderr, new RegExp(`backend\\.${key} is not allowed`));
    });
  }
});

test('rejects malformed backend values and executable paths', async () => {
  const cases = [
    { backend: [], pattern: /backend must be an object/ },
    { backend: 'backend', pattern: /backend must be an object/ },
    { backend: {}, pattern: /backend\.executable is required/ },
    { backend: { executable: '' }, pattern: /backend\.executable is required/ },
    { backend: { executable: '/usr/bin/app' }, pattern: /relative path inside the package/ },
    { backend: { executable: '../backend/app' }, pattern: /normalized relative path/ },
    { backend: { executable: 'backend/../app' }, pattern: /normalized relative path/ },
    { backend: { executable: './backend/app' }, pattern: /normalized relative path/ },
    { backend: { executable: 'backend//app' }, pattern: /normalized relative path/ },
    { backend: { executable: 'backend\\app' }, pattern: /forward slashes/ },
    { backend: { executable: 'backend/\u0000app' }, pattern: /control characters/ },
    { backend: { executable: 'a'.repeat(4097) }, pattern: /at most 4096 bytes/ },
    { backend: { executable: 'backend/app', args: 'text' }, pattern: /backend\.args must be an array/ },
    { backend: { executable: 'backend/app', args: [42] }, pattern: /backend\.args\[0\] must be a string/ },
    { backend: { executable: 'backend/app', args: ['x\u0000y'] }, pattern: /must not contain NUL or control/ },
    { backend: { executable: 'backend/app', args: ['x\u0001y'] }, pattern: /must not contain NUL or control/ },
    { backend: { executable: 'backend/app', args: ['x'.repeat(8193)] }, pattern: /at most 8192 bytes/ },
    {
      backend: { executable: 'backend/app', args: Array.from({ length: 17 }, () => 'x'.repeat(8000)) },
      pattern: /must total at most 131072 bytes/,
    },
    {
      backend: { executable: 'backend/app', args: Array.from({ length: 129 }, () => 'x') },
      pattern: /at most 128 entries/,
    },
  ];
  for (const { backend, pattern } of cases) {
    await withApp({ backend }, async (root) => {
      const result = await run(['validate', root]);
      assert.notEqual(result.code, 0, `expected rejection for ${JSON.stringify(backend).slice(0, 80)}`);
      assert.match(result.stderr, pattern);
    });
  }
});

test('rejects staged backend files that are missing, non-executable or not ELF', async () => {
  const cases = [
    {
      setup: async () => {},
      pattern: /does not exist in the package/,
    },
    {
      setup: async (root) => writeBackend(root, { mode: 0o644 }),
      pattern: /is not executable/,
    },
    {
      setup: async (root) => writeBackend(root, { contents: Buffer.from('#!/bin/sh\necho hi\n') }),
      pattern: /not an ELF file/,
    },
    {
      setup: async (root) =>
        writeBackend(root, { contents: Buffer.from([0x7f, 0x45, 0x4c, 0x46, 0x02, 0x01, 0x01, 0x00]) }),
      pattern: /too short/,
    },
    {
      setup: async (root) => writeBackend(root, { contents: elfHeader({ machine: 0x1234 }) }),
      pattern: /unsupported ELF architecture/,
    },
    {
      setup: async (root) => writeBackend(root, { contents: elfHeader({ machine: 183, elfClass: 1 }) }),
      pattern: /does not match aarch64/,
    },
    {
      setup: async (root) => writeBackend(root, { contents: elfHeader({ bigEndian: true }) }),
      pattern: /big-endian/,
    },
    {
      setup: async (root) => writeBackend(root, { contents: elfHeader({ type: 1 }) }),
      pattern: /ET_EXEC/,
    },
  ];
  for (const { setup, pattern } of cases) {
    await withApp({ backend: { executable: 'backend/notes-backend' } }, async (root) => {
      await setup(root);
      const result = await run(['validate', root]);
      assert.notEqual(result.code, 0, `expected rejection for ${pattern}`);
      assert.match(result.stderr, pattern);
    });
  }
});

test('warns without failing when the ELF targets another architecture', async () => {
  const target = process.arch === 'arm64' ? { machine: 62, elfClass: 2 } : { machine: 183, elfClass: 2 };
  const expected = target.machine === 62 ? 'x86_64' : 'aarch64';
  await withApp({ backend: { executable: 'backend/notes-backend' } }, async (root) => {
    await writeBackend(root, { contents: elfHeader(target) });
    const result = await run(['validate', root]);
    assert.equal(result.code, 0, result.stderr);
    assert.match(result.stdout, new RegExp(`targets ${expected}`));
    assert.match(result.stdout, /architecture/);
  });
});

test('native backend template contains the documented files', async () => {
  const files = [
    'manifest.json',
    'index.html',
    'app.js',
    'styles.css',
    'vendor/edgedesk-sdk.js',
    'backend/main.c',
    'backend/Makefile',
    'README.md',
  ];
  for (const file of files) {
    const stat = await fs.stat(path.join(template, file)).catch(() => null);
    assert.ok(stat?.isFile(), `missing template file: ${file}`);
  }
  const manifest = JSON.parse(await fs.readFile(path.join(template, 'manifest.json'), 'utf8'));
  assert.deepEqual(manifest.backend, { executable: 'backend/notes-backend' });
});

test('init scaffolds the native backend template and refuses non-empty destinations', async () => {
  const root = await tempDir();
  try {
    const destination = path.join(root, 'app');
    const created = await run(['init', destination, '--template', 'native-backend']);
    assert.equal(created.code, 0, created.stderr);
    assert.match(created.stdout, /created:/);
    for (const file of ['manifest.json', 'backend/main.c', 'backend/Makefile', 'vendor/edgedesk-sdk.js']) {
      assert.ok((await fs.stat(path.join(destination, file))).isFile(), `missing scaffold file: ${file}`);
    }

    const again = await run(['init', destination, '--template', 'native-backend']);
    assert.notEqual(again.code, 0);
    assert.match(again.stderr, /not empty/);

    const unknown = await run(['init', path.join(root, 'other'), '--template', 'missing']);
    assert.notEqual(unknown.code, 0);
    assert.match(unknown.stderr, /unknown template/);
  } finally {
    await fs.rm(root, { recursive: true, force: true });
  }
});

test('help and inspect describe backend support', async () => {
  const help = await run(['--help']);
  assert.equal(help.code, 0);
  assert.match(help.stdout, /native-backend/);
  assert.match(help.stdout, /backend\.executable/);
});
