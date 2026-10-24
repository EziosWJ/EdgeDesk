# EdgeDesk Extension SDK 1.0

Independent development toolkit for EdgeDesk applications. Node.js is used
only as a development/build tool; EdgeDesk devices never execute Node.js or any
script shipped in a package. A package contains static Web assets and,
optionally, one native ELF backend (SPEC-0011).

## Package layout

```text
manifest.json       required, stable reverse-domain id + SemVer
index.html          required entry
assets/...          built static resources, relative paths only
backend/<name>      optional executable ELF (one architecture per package)
```

No symlinks, absolute paths, `..`, duplicate (case-insensitive) names or
script runtime payloads are accepted. ZIP packages are uploaded by an
administrator and installed as immutable internal revisions.

## CLI

```bash
# Validate manifest, assets, backend path/ELF and relative references
node bin/edgedesk-ext.js validate ./dist

# Validate and create a ZIP package (keeps the executable bit)
node bin/edgedesk-ext.js pack ./dist my-app.edgedesk.zip

# Inspect an existing package's entry list (marks executable entries)
node bin/edgedesk-ext.js inspect my-app.edgedesk.zip

# Scaffold from a template
node bin/edgedesk-ext.js init ./my-app --template native-backend
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

const health = await client.backendRequest('GET', '/health');
const result = await client.backendRequest('POST', '/notes', { body: { text: 'hello' } });

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

## Native backend packages (SPEC-0011)

A package may add one optional native backend. It is never executed during
installation; EdgeDesk validates it, copies it to a stable program path and
lets an administrator start it through the Process Supervisor.

### Manifest

```json
"backend": {
  "executable": "backend/notes-backend",
  "args": ["--mode", "notes"]
}
```

| Field | Rule |
| --- | --- |
| `backend` | Optional object. Missing = Web-only app with unchanged behavior. |
| `backend.executable` | Required normalized relative path, max 4096 bytes. Rejected: absolute path, `..`, `.`, empty segment, backslash, NUL/control characters. |
| `backend.args` | Optional string array: max 128 entries, max 8192 bytes each, max 128 KiB total. Each entry is one argv element; there is no shell expansion. |

All other `backend` keys are rejected, including `run_user`, `run_group`,
`root`, `autostart`, `socket`, `data_dir`, `url` and `port`. Identity,
autostart, socket and data directory are administrator/host properties.

The SDK validator checks that the executable exists, is a regular file, has
the executable bit set and has a supported ELF header (x86_64, aarch64, arm32;
ET_EXEC/ET_DYN, little-endian). It warns when the target architecture differs
from the validator host. The **device performs the authoritative ELF,
executable-mode and architecture checks at install** and rejects scripts,
non-ELF files, symlinks and mismatched architectures.

### Runtime contract

- `EDGEDESK_APP_SOCKET`: absolute Unix socket path supplied by the host.
- `EDGEDESK_APP_DATA_DIR`: absolute persistent data directory supplied by the
  host. The manifest cannot override either variable.
- The backend binds a `SOCK_STREAM` UDS and answers HTTP/1.1 with
  `Content-Length`. V1 proxies JSON `GET|POST|PUT|PATCH|DELETE` requests only;
  no WebSocket, streaming, arbitrary TCP or cross-app access.
- The sandboxed UI calls `client.backendRequest(method, path, { body })`; the
  host resolves the socket from the authenticated runtime instance, so the page
  can never name a host, port or socket.
- Run the process as an administrator-selected user/group. The program
  directory is not writable by that user; persistent data belongs under
  `EDGEDESK_APP_DATA_DIR`. The socket uses mode `0660` and the run directory is
  created by EdgeDesk; unsafe permissions cause startup to be refused.
- Installation and upgrade leave the backend stopped and autostart off.
  Native upgrades overwrite the stable program path; there is **no automatic
  rollback**. A failed upgrade keeps the app stopped and marks it
  `UPGRADE_FAILED`. Uninstall keeps data by default.

### Package layout

```text
manifest.json
index.html
... static assets ...
backend/notes-backend     ELF executable, mode 0755
```

One package targets exactly one architecture. Build and publish x86_64,
aarch64 and arm32 packages separately; a package for another architecture is
rejected by the device.

### Unsigned packages

Signatures are not required in V1. SHA-256 integrity entries detect corruption
and content changes but do not prove the publisher. Install only packages from
a trusted source; a backend runs with the OS permissions of the selected
identity (including host-level access if an administrator selects root).

See `templates/native-backend` for a complete C example, build/cross-compile
commands and the executed-versus-unverified evidence notes.

## Templates

- `templates/native-backend` — static UI + POSIX C backend (SPEC-0011)
- `templates/vanilla` — no build step, directly packable
- `templates/react` — React + Vite
- `templates/vue` — Vue + Vite
- `templates/svelte` — Svelte + Vite

`edgedesk-ext init <dir> --template <name>` copies any of these. Each template
vendors `vendor/edgedesk-sdk.js` so it can be built offline. Framework
templates require their own `npm install` in the template directory; the SDK
itself has no runtime dependencies. `native-backend` additionally requires a C
compiler (`make`) and must be built before `validate`/`pack`.
