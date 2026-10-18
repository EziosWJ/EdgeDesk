import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promises as fs } from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import test from 'node:test';

const here = path.dirname(fileURLToPath(import.meta.url));
const cli = path.join(here, '..', 'bin', 'edgedesk-ext.js');

function run(args, cwd) {
  return new Promise((resolve) => {
    execFile(process.execPath, [cli, ...args], { cwd }, (error, stdout, stderr) => {
      resolve({ code: error ? error.code ?? 1 : 0, stdout, stderr });
    });
  });
}

async function makeApp(root, overrides = {}) {
  const manifest = {
    id: 'io.example.cli',
    name: 'CLI Test',
    version: '1.0.0',
    entry: 'index.html',
    minHostApiVersion: '1.0.0',
    maxHostApiVersion: '1.9.9',
    permissions: ['storage'],
    ...overrides,
  };
  await fs.mkdir(root, { recursive: true });
  await fs.writeFile(path.join(root, 'manifest.json'), JSON.stringify(manifest));
  await fs.writeFile(path.join(root, 'index.html'), '<!doctype html><script type="module" src="./main.js"></script>');
  await fs.writeFile(path.join(root, 'main.js'), 'console.log("cli")');
  return manifest;
}

async function tempDir() {
  return fs.mkdtemp(path.join(os.tmpdir(), 'edgedesk-cli-test-'));
}

test('validate and pack a static app', async () => {
  const root = await tempDir();
  await makeApp(root);
  const validated = await run(['validate', root]);
  assert.equal(validated.code, 0, validated.stderr);
  assert.match(validated.stdout, /valid: io\.example\.cli 1\.0\.0/);
  const output = path.join(root, 'app.zip');
  const packed = await run(['pack', root, output]);
  assert.equal(packed.code, 0, packed.stderr);
  const inspected = await run(['inspect', output]);
  assert.equal(inspected.code, 0, inspected.stderr);
  assert.match(inspected.stdout, /manifest\.json/);
  assert.match(inspected.stdout, /index\.html/);
  await fs.rm(root, { recursive: true, force: true });
});

test('rejects invalid manifest ids and traversal', async () => {
  const root = await tempDir();
  await makeApp(root, { id: 'Not-Reverse-Domain' });
  const result = await run(['validate', root]);
  assert.notEqual(result.code, 0);
  assert.match(result.stderr, /reverse-domain/);
  await fs.rm(root, { recursive: true, force: true });
});

test('rejects absolute asset references and symlinks', async () => {
  const root = await tempDir();
  await makeApp(root);
  await fs.writeFile(path.join(root, 'index.html'), '<img src="/absolute.png">');
  const absolute = await run(['validate', root]);
  assert.notEqual(absolute.code, 0);
  assert.match(absolute.stderr, /absolute path/);
  await fs.writeFile(path.join(root, 'index.html'), '<h1>ok</h1>');
  await fs.symlink('/etc/passwd', path.join(root, 'link'));
  const symlink = await run(['validate', root]);
  assert.notEqual(symlink.code, 0);
  assert.match(symlink.stderr, /symlink/);
  await fs.rm(root, { recursive: true, force: true });
});

test('rejects case-insensitive duplicate entries', async () => {
  const root = await tempDir();
  await makeApp(root);
  await fs.writeFile(path.join(root, 'Asset.js'), 'a');
  await fs.writeFile(path.join(root, 'asset.js'), 'b');
  const result = await run(['validate', root]);
  assert.notEqual(result.code, 0);
  assert.match(result.stderr, /duplicate/);
  await fs.rm(root, { recursive: true, force: true });
});
