#!/usr/bin/env node
/**
 * Browser boundary check for extension network permissions.
 *
 * Playwright's npm package is not vendored in this repository's offline test
 * lane, so this script drives the already-installed headless Chromium over the
 * Chrome DevTools Protocol using only Node built-ins. It serves a host page
 * with the same iframe sandbox flags as ExtensionAppView and checks, from the
 * sandboxed document:
 *
 *   - approved origins are usable for fetch/img/iframe when present in CSP
 *   - undeclared origins are blocked for fetch/img/iframe/form
 *   - popup, target=_blank and frame self-navigation are NOT constrained by CSP
 *
 * Usage:
 *   node web/scripts/extension-network-boundary.mjs
 *
 * Environment:
 *   CHROME  path to a Chromium/headless-shell binary. Defaults to the first
 *           the first chromium_headless_shell directory in the Playwright cache.
 */
import { spawn } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import os from 'node:os';
import path from 'node:path';

const APP_PORT = 19180;
const NET_PORT = 19181;
const CDP_PORT = 19333;
const HOST_ORIGIN = `http://127.0.0.1:${APP_PORT}`;
const APPROVED_ORIGIN = `http://127.0.0.1:${NET_PORT}`;
const UNDECLARED_ORIGIN = `http://localhost:${NET_PORT}`;

function findChrome() {
  if (process.env.CHROME) return process.env.CHROME;
  const root = path.join(os.homedir(), '.cache', 'ms-playwright');
  for (const entry of fs.readdirSync(root)) {
    if (!entry.startsWith('chromium_headless_shell-')) continue;
    const candidate = path.join(root, entry, 'chrome-headless-shell-linux64', 'chrome-headless-shell');
    if (fs.existsSync(candidate)) return candidate;
  }
  throw new Error('headless Chromium not found; set CHROME');
}

const csp = (allowApproved) => {
  const external = allowApproved ? ` ${APPROVED_ORIGIN}` : '';
  return [
    "default-src 'self'",
    `script-src 'self' ${HOST_ORIGIN}${external}`,
    `style-src 'self' 'unsafe-inline' ${HOST_ORIGIN}${external}`,
    `img-src 'self' data: ${HOST_ORIGIN}${external}`,
    `font-src 'self' data: ${HOST_ORIGIN}${external}`,
    `connect-src 'self' ${HOST_ORIGIN}${external}`,
    `media-src 'self' data: ${HOST_ORIGIN}${external}`,
    `frame-src 'self' ${HOST_ORIGIN}${external}`,
    `child-src 'self' ${HOST_ORIGIN}${external}`,
    `form-action 'self' ${HOST_ORIGIN}${external}`,
    "object-src 'none'; base-uri 'none'; frame-ancestors 'self'",
  ].join('; ');
};

const APP_JS = `
const APPROVED = ${JSON.stringify(APPROVED_ORIGIN)};
const UNDECLARED = ${JSON.stringify(UNDECLARED_ORIGIN)};
const results = {};
const note = (key, value) => { results[key] = value; };
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

async function fetchProbe(key, url) {
  try {
    const response = await fetch(url);
    note(key, 'status:' + response.status);
  } catch (error) {
    note(key, 'blocked:' + error.name);
  }
}

function imageProbe(key, url) {
  return new Promise((resolve) => {
    const image = new Image();
    let settled = false;
    const finish = (value) => {
      if (settled) return;
      settled = true;
      note(key, value);
      resolve();
    };
    image.onload = () => finish('loaded');
    image.onerror = () => finish('blocked');
    image.src = url;
    setTimeout(() => finish('timeout'), 2000);
  });
}

function iframeProbe(url) {
  const frame = document.createElement('iframe');
  frame.src = url;
  frame.style.display = 'none';
  document.body.appendChild(frame);
}

async function run() {
  await fetchProbe('approvedFetch', APPROVED + '/approved-fetch');
  await fetchProbe('undeclaredFetch', UNDECLARED + '/undeclared-fetch');
  await imageProbe('approvedImage', APPROVED + '/approved-image');
  await imageProbe('undeclaredImage', UNDECLARED + '/undeclared-image');

  iframeProbe(APPROVED + '/approved-frame');
  iframeProbe(UNDECLARED + '/undeclared-frame');

  try {
    const popup = window.open(UNDECLARED + '/undeclared-popup', '_blank');
    note('windowOpen', popup ? 'opened' : 'blocked');
  } catch (error) {
    note('windowOpen', 'error:' + error.name);
  }

  const anchor = document.createElement('a');
  anchor.href = UNDECLARED + '/undeclared-anchor';
  anchor.target = '_blank';
  anchor.textContent = 'anchor';
  document.body.appendChild(anchor);
  anchor.click();
  note('anchorBlank', 'clicked');

  const form = document.createElement('form');
  form.method = 'GET';
  form.action = UNDECLARED + '/undeclared-form';
  form.target = '_blank';
  document.body.appendChild(form);
  try {
    form.submit();
    note('formSubmit', 'submitted');
  } catch (error) {
    note('formSubmit', 'error:' + error.name);
  }

  await wait(2500);
  note('locationBefore', String(window.location.href));
  parent.postMessage({ __boundary: true, results }, '*');
  await wait(100);
  try {
    window.location.href = UNDECLARED + '/undeclared-selfnav';
    note('selfNavigation', 'requested');
  } catch (error) {
    note('selfNavigation', 'error:' + error.name);
  }
}

run();
`;

const hostHtml = `<!doctype html><html><body>
<script>
window.__boundary = null;
addEventListener('message', (event) => {
  if (event.data && event.data.__boundary) window.__boundary = event.data.results;
});
const frame = document.createElement('iframe');
frame.id = 'app';
frame.sandbox = 'allow-scripts allow-forms allow-downloads allow-popups allow-popups-to-escape-sandbox allow-modals';
frame.src = '/app/index.html?allow=' + new URLSearchParams(location.search).get('allow');
document.body.appendChild(frame);
</script></body></html>`;

function createHandler() {
  const hits = new Map();
  const bump = (name) => hits.set(name, (hits.get(name) ?? 0) + 1);
  const handler = (request, response) => {
    const url = new URL(request.url, HOST_ORIGIN);
    const route = url.pathname;
    if (route === '/host.html') {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      response.end(hostHtml);
      return;
    }
    if (route === '/app/index.html') {
      const allow = url.searchParams.get('allow') === '1';
      response.writeHead(200, {
        'Content-Type': 'text/html; charset=utf-8',
        'Content-Security-Policy': csp(allow),
      });
      response.end('<!doctype html><html><body><script src="/app/app.js"></script></body></html>');
      return;
    }
    if (route === '/app/app.js') {
      response.writeHead(200, { 'Content-Type': 'text/javascript; charset=utf-8' });
      response.end(APP_JS);
      return;
    }
    bump(route.slice(1).replaceAll('/', '-') || 'root');
    if (route.endsWith('-frame') || route.endsWith('-popup') || route.endsWith('-anchor') ||
        route.endsWith('-form') || route.endsWith('-selfnav')) {
      response.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
      response.end('<!doctype html><html><body>external</body></html>');
      return;
    }
    response.writeHead(200, { 'Content-Type': 'image/gif' });
    response.end(Buffer.from('R0lGODlhAQABAAAAACw=', 'base64'));
  };
  return { handler, hits };
}

async function waitFor(url, attempts = 100) {
  for (let i = 0; i < attempts; ++i) {
    try {
      const response = await fetch(url);
      if (response.ok) return response;
    } catch {
      // retry
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('timed out waiting for ' + url);
}

async function openPage(hostUrl) {
  const response = await fetch(`http://127.0.0.1:${CDP_PORT}/json/new`, { method: 'PUT' });
  const target = await response.json();
  const ws = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = reject;
  });
  let nextId = 0;
  const pending = new Map();
  ws.onmessage = (event) => {
    const message = JSON.parse(event.data);
    if (message.method === 'Runtime.exceptionThrown') {
      console.error('[cdp] exception:', JSON.stringify(message.params.exceptionDetails?.text ?? ''));
    }
    if (message.id && pending.has(message.id)) {
      const { resolve, reject } = pending.get(message.id);
      pending.delete(message.id);
      if (message.error) reject(new Error(JSON.stringify(message.error)));
      else resolve(message.result);
    }
  };
  const send = (method, params = {}) =>
    new Promise((resolve, reject) => {
      const id = ++nextId;
      pending.set(id, { resolve, reject });
      ws.send(JSON.stringify({ id, method, params }));
    });
  await send('Page.enable');
  await send('Runtime.enable');
  await send('Log.enable');
  await send('Page.navigate', { url: hostUrl });
  return {
    async readResults(timeoutMs = 20000) {
      const deadline = Date.now() + timeoutMs;
      for (;;) {
        const result = await send('Runtime.evaluate', {
          expression: 'window.__boundary',
          returnByValue: true,
        });
        if (result.result?.value) return result.result.value;
        if (Date.now() > deadline) {
          const state = await send('Runtime.evaluate', {
            expression: `JSON.stringify({href: location.href, ready: document.readyState,
              body: document.body ? document.body.innerHTML.slice(0, 300) : null,
              frames: window.frames.length})`,
            returnByValue: true,
          });
          throw new Error('boundary results timed out: ' + state.result?.value);
        }
        await new Promise((resolve) => setTimeout(resolve, 200));
      }
    },
    async close() {
      ws.close();
      await fetch(`http://127.0.0.1:${CDP_PORT}/json/close/${target.id}`);
    },
    send,
  };
}

async function runScenario(label, allowApproved) {
  const { handler, hits } = createHandler();
  const hostServer = http.createServer(handler);
  const externalServer = http.createServer(handler);
  await new Promise((resolve) => hostServer.listen(APP_PORT, '127.0.0.1', resolve));
  await new Promise((resolve) => externalServer.listen(NET_PORT, '127.0.0.1', resolve));
  try {
    const page = await openPage(`${HOST_ORIGIN}/host.html?allow=${allowApproved ? 1 : 0}`);
    const results = await page.readResults();
    // The probe requests a top-level navigation ~100 ms after posting its
    // results. Give the request time to reach the external server before
    // closing the page; otherwise the hit list races the navigation.
    await new Promise((resolve) => setTimeout(resolve, 1500));
    await page.close();
    const hitList = Object.fromEntries(hits);
    console.log(`\n[${label}] iframe results:`, JSON.stringify(results, null, 2));
    console.log(`[${label}] external origin hits:`, JSON.stringify(hitList, null, 2));
    return { results, hits: hitList };
  } finally {
    hostServer.closeAllConnections?.();
    externalServer.closeAllConnections?.();
    await new Promise((resolve) => hostServer.close(resolve));
    await new Promise((resolve) => externalServer.close(resolve));
  }
}

const chromePath = findChrome();
const profile = fs.mkdtempSync(path.join(os.tmpdir(), 'edgedesk-cdp-'));
const chrome = spawn(chromePath, [
  '--headless',
  '--no-sandbox',
  '--disable-gpu',
  '--disable-popup-blocking',
  '--no-first-run',
  `--remote-debugging-port=${CDP_PORT}`,
  `--user-data-dir=${profile}`,
  'about:blank',
], { stdio: 'ignore' });

try {
  await waitFor(`http://127.0.0.1:${CDP_PORT}/json/version`);
  const denied = await runScenario('network denied', false);
  const allowed = await runScenario('network approved', true);

  const failures = [];
  const expect = (condition, message) => {
    if (!condition) failures.push(message);
  };
  expect(denied.hits['approved-fetch'] === undefined, 'denied: approved origin fetch should be blocked');
  expect(denied.hits['approved-image'] === undefined, 'denied: approved origin image should be blocked');
  expect(denied.hits['approved-frame'] === undefined, 'denied: approved origin iframe should be blocked');
  expect(allowed.hits['approved-fetch'] === 1, 'approved: approved origin fetch should succeed');
  expect(allowed.hits['approved-image'] === 1, 'approved: approved origin image should succeed');
  expect(allowed.hits['approved-frame'] === 1, 'approved: approved origin iframe should succeed');
  for (const scenario of [denied, allowed]) {
    for (const route of ['undeclared-fetch', 'undeclared-image', 'undeclared-frame', 'undeclared-form']) {
      expect(scenario.hits[route] === undefined, `${scenario === denied ? 'denied' : 'approved'}: ${route} should be blocked`);
    }
  }
  // Residual risk: popups escape the sandbox, target=_blank and frame
  // self-navigation are not constrained by CSP, and the SDK validator is not a
  // security boundary. These hits are the documented browser behavior.
  for (const scenario of [denied, allowed]) {
    for (const route of ['undeclared-popup', 'undeclared-anchor', 'undeclared-selfnav']) {
      expect(scenario.hits[route] === 1, `${scenario === denied ? 'denied' : 'approved'}: expected ${route} navigation escape`);
    }
  }

  if (failures.length > 0) {
    console.error('\nBOUNDARY FAILURES:');
    for (const failure of failures) console.error(' - ' + failure);
    process.exitCode = 1;
  } else {
    console.log('\nnetwork boundary: PASS (CSP blocks resources; popup/anchor escape confirmed as residual risk)');
  }
} finally {
  chrome.kill('SIGKILL');
  fs.rmSync(profile, { recursive: true, force: true });
}
