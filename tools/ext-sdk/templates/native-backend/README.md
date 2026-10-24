# Native backend template

A static Web UI plus one native C backend, packaged as a single ZIP. The
backend is a plain POSIX program (libc only); EdgeDesk never runs a shell or a
scripting runtime for it.

## Quick start

```bash
# 1. Scaffold (or copy this directory)
node bin/edgedesk-ext.js init ./notes-app --template native-backend

# 2. Build the backend for the device architecture
cd notes-app/backend && make

# 3. Validate and pack
cd ..
node ../bin/edgedesk-ext.js validate .
node ../bin/edgedesk-ext.js pack . notes-app.edgedesk.zip
```

`validate` and `pack` reject the package until `backend/notes-backend` exists,
is executable and is an ELF file whose header matches a supported architecture
(x86_64, aarch64, arm32).

## manifest.json

```json
{
  "id": "dev.edgedesk.notes-example",
  "name": "Notes Backend Example",
  "version": "1.0.0",
  "entry": "index.html",
  "minHostApiVersion": "1.0.0",
  "maxHostApiVersion": "1.9.9",
  "permissions": [],
  "backend": {
    "executable": "backend/notes-backend",
    "args": []
  }
}
```

| Field | Rule |
| --- | --- |
| `backend` | Optional object. Missing means a Web-only app; nothing in the ZIP is executed. |
| `backend.executable` | Required. Normalized relative package path, at most 4096 bytes. No absolute path, `..`, `.`, empty segment, backslash, NUL or control character. |
| `backend.args` | Optional array of strings, at most 128 entries, at most 8192 bytes each and 128 KiB total. Each entry is one argv element; no shell expansion. |

`backend` rejects every other key. Run user, run group, `allow_root`,
autostart, socket path, data directory, URL and port are configured by the
EdgeDesk administrator or host, never by the package.

Only one backend executable per package. `backend.executable` must be a
regular, executable ELF file for the device architecture. The SDK validator
checks the ELF header and executable bit locally; the device re-checks the
full file and the architecture at install time and rejects scripts, non-ELF
files, symlinks and mismatched architectures.

## Package layout

```text
manifest.json
index.html
app.js
styles.css
vendor/edgedesk-sdk.js
backend/notes-backend      executable ELF, mode 0755
backend/main.c             optional source; harmless if shipped
backend/Makefile
```

The executable must be inside the package and is copied by EdgeDesk to a
stable per-application program path. The package is never executed during
installation.

## Environment contract

EdgeDesk starts the executable with an argv array and sets exactly these
application variables:

| Variable | Meaning |
| --- | --- |
| `EDGEDESK_APP_SOCKET` | Absolute Unix domain socket path the host will connect to. |
| `EDGEDESK_APP_DATA_DIR` | Absolute persistent data directory for this app. |

Both are host-managed and cannot be overridden by the manifest. The working
directory is the stable program directory; persistent business data must be
written under `EDGEDESK_APP_DATA_DIR`, never next to the executable (an upgrade
replaces program files but leaves the data directory intact).

## Socket and HTTP contract

- Bind a `SOCK_STREAM` Unix domain socket at `EDGEDESK_APP_SOCKET`. The path is
  at most 107 bytes and the host creates the parent run directory.
- The host sends one HTTP/1.1 request per connection and closes it; responses
  must include `Content-Length`. `Connection: close` is recommended.
- Only JSON request/response bodies are proxied in V1. WebSocket, streaming
  responses, arbitrary TCP and cross-application calls are not available.
- The host rejects hop-by-hop headers and forwards only `application/json`
  payloads to `backend.request`.
- Requests arrive with `GET|POST|PUT|PATCH|DELETE` and an application-relative
  path starting with `/`. The host fixes the socket from the authenticated
  runtime instance; the frontend cannot choose a host, port or URL.
- Do not listen on TCP. The Web UI reaches the backend only through the host
  JSON-RPC bridge.

The example implements:

| Route | Response |
| --- | --- |
| `GET /health` | `{"ok":true,"app":"example"}` |
| `GET /notes` | `{"notes":["..."]}` |
| `POST /notes` | request `{"text":"..."}`, response `201 {"ok":true}` |

The example serves one request at a time with fixed 16 KiB header, 1 MiB body
and 256 KiB notes limits, and writes `<data>/notes.txt` with `openat` and
`O_NOFOLLOW`.

## Identity and socket permissions

- The administrator selects the run user/group after installation. The package
  cannot request root or a specific identity.
- The run directory is created and owned by EdgeDesk. The backend creates the
  socket and sets `0660`; the EdgeDesk host connects as a member of the shared
  group. If the run directory permissions do not allow both sides, startup is
  refused by the host rather than silently weakened.
- The program directory is not writable by the backend run user. Data goes to
  `EDGEDESK_APP_DATA_DIR`.

## Build and cross-compilation

Native build (executed for this template on x86_64 Linux):

```bash
cd backend
make                    # uses cc, produces ELF backend/notes-backend
```

Cross-builds produce a package for a different device. These commands are
documented, **not executed in this repository**:

```bash
# ARM64 (aarch64)
make clean && make CC=aarch64-linux-gnu-gcc

# ARM32 hard-float
make clean && make CC=arm-linux-gnueabihf-gcc
```

One package contains exactly one architecture. Build and publish x86_64,
aarch64 and arm32 packages separately; a package for the wrong architecture is
rejected at install. A successful cross-compile is not a substitute for
running the backend on the target device.

A fully static binary avoids libc version skew on old targets:

```bash
make LDFLAGS=-static
```

## Upgrade, rollback and uninstall

- Upgrade validates the whole ZIP first, then stops the backend and overwrites
  the stable executable path. There are no version directories and **no
  automatic rollback** of native binaries.
- If replacement or metadata commit fails after the stop, the app remains
  stopped and is marked `UPGRADE_FAILED`; an administrator must upload a known
  good package or restore manually.
- Upgrading does not delete `EDGEDESK_APP_DATA_DIR` or platform KV data. The app
  owns its data migration.
- Uninstall stops the backend and removes program files; data and KV are kept
  by default. Data removal is a separate administrator operation.
- After installation the backend is stopped and autostart is off. Start it
  explicitly.

## Unsigned packages

EdgeDesk does not require digital signatures. SHA-256 integrity entries detect
corruption or content changes but do **not** prove who built the package. Only
install packages from a source you trust; a backend runs with the OS
permissions of the administrator-selected identity.

## Tests and evidence

The SDK test suite validates manifest acceptance/rejection cases, executable
mode preservation in ZIP entries and template presence:

```bash
cd tools/ext-sdk && npm test
```

Executed in this repository on x86_64 Linux: native `make` build of the
example backend (`cc` 13.3.0, glibc 2.39; `ldd` shows only libc), and a manual
UDS run of `GET /health`, `GET /notes`, `POST /notes`, an invalid body
(HTTP 400) and an unknown route (HTTP 404). Cross-compiled ARM64/ARM32 binaries
and on-device installation were not executed.
