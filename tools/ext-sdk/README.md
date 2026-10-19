# EdgeDesk Extension SDK 1.0

Independent development toolkit for EdgeDesk static Web applications. Node.js
is used only as a development/build tool; EdgeDesk devices never execute
Node.js, and installed packages contain static assets only.

## Package layout

```text
manifest.json       required, stable reverse-domain id + SemVer
index.html          required entry
assets/...          built static resources, relative paths only
```

No symlinks, absolute paths, `..`, duplicate (case-insensitive) names or
backend/script runtime payloads are accepted. ZIP packages are uploaded by an
administrator and installed as immutable internal revisions.

## CLI

```bash
# Validate manifest, assets and relative references
node bin/edgedesk-ext.js validate ./dist

# Validate and create a ZIP package
node bin/edgedesk-ext.js pack ./dist my-app.edgedesk.zip

# Inspect an existing package's entry list
node bin/edgedesk-ext.js inspect my-app.edgedesk.zip
```

Build a framework template with Vite first (`npm run build`), then run the CLI
against its `dist/` directory. The CLI requires Node.js 20+ and has no npm
dependencies.

## SDK

```js
import { connect, installErrorReporting } from './vendor/edgedesk-sdk.js';

const client = await connect();
installErrorReporting(client);

const info = await client.getInfo();
const app = await client.storageGet('settings').catch(() => null);
await client.storageSet('settings', { theme: 'dark' });

const listing = await client.filesList('/');
const entry = await client.uploadFile('/report.bin', file);
const blob = await client.downloadFile('/report.bin');

const session = await client.terminalCreate({ title: 'App shell' });
client.on('terminal.output', ({ sessionId, data }) => {
  if (sessionId === session.id) console.log(data);
});
await client.terminalInput(session.id, 'ls\r');
```

- `connect()` performs the postMessage handshake and waits for a dedicated
  MessagePort. It fails when the page is not hosted by EdgeDesk.
- `call()` uses JSON-RPC 2.0 request IDs. Its optional timeout is a caller-side
  waiting timeout; it does not prove the host cancelled the operation.
- Errors are `RpcError` with `code`, `message` and optional `data.slug`.
- Upload/download use 1 MiB binary chunks over a dedicated transfer port with
  per-chunk acknowledgement. Transfers are not resumable.
- `openExternal()` only opens declared network origins and returns a callback
  validator that checks `event.source`, origin, random nonce and state, and
  message shape. OAuth-style callbacks must use this pattern; never execute
  host API commands received through `window.opener`. This is a developer aid,
  not a security boundary: direct `window.open`, `target="_blank"` and iframe
  self-navigation are not restricted by the network grant (ADR-0014).
- Browser `localStorage` and IndexedDB are not supported persistence
  contracts. Use the storage API.
- The iframe never receives host cookies, bearer tokens or absolute paths.

## Permissions

| Module | Capabilities |
| --- | --- |
| `storage` | isolated JSON KV, 5 MiB/app, 256 KiB/key, transactions |
| `files.manage` | virtual-path file metadata, read/write/delete, chunked transfer |
| `terminal.access` | PTY session create/list/input/resize/close |
| `network` | declared external origins (CSP), approved by the administrator |

All permissions default to denied and are approved per module. New permissions
introduced by an upgrade require a new approval. A permission revocation is
applied to the next running instance; disabling the app terminates instances
immediately.

## Host API compatibility

The host API version is independent of the EdgeDesk product version. This SDK
targets host API `1.0.0`. `manifest.json` must declare
`minHostApiVersion`/`maxHostApiVersion`; missing, malformed or out-of-range
values prevent the app from being enabled.

## Templates

- `templates/vanilla` — no build step, directly packable
- `templates/react` — React + Vite
- `templates/vue` — Vue + Vite
- `templates/svelte` — Svelte + Vite

Each template vendors `vendor/edgedesk-sdk.js` so it can be built offline.
Framework templates require their own `npm install` in the template directory;
the SDK itself has no runtime dependencies.
