# Native backend notes example

A complete, packable EdgeDesk application with one optional native backend:

```text
manifest.json          static Web app + backend.executable
index.html, app.js     sandboxed UI, calls client.backendRequest()
styles.css
vendor/                vendored EdgeDesk browser SDK (development aid)
backend/main.c         POSIX HTTP/1.1 server over a Unix domain socket
backend/Makefile       native build; cross-compile targets documented below
```

This example is also available as the SDK scaffold
`tools/ext-sdk/templates/native-backend`:

```bash
node tools/ext-sdk/bin/edgedesk-ext.js init /tmp/notes-app --template native-backend
```

The detailed manifest, environment, socket, permission, upgrade and
cross-compilation contract lives in the template README:
[`tools/ext-sdk/templates/native-backend/README.md`](../../tools/ext-sdk/templates/native-backend/README.md).
The SDK overview is in [`tools/ext-sdk/README.md`](../../tools/ext-sdk/README.md).

## Build and pack

```bash
cd examples/native-backend-notes/backend
make                       # native build -> backend/notes-backend (ELF, 0755)
cd ..
node ../../tools/ext-sdk/bin/edgedesk-ext.js validate .
node ../../tools/ext-sdk/bin/edgedesk-ext.js pack . /tmp/native-backend-notes.edgedesk.zip
```

`validate` fails while `backend/notes-backend` is missing or not executable.
The package contains one architecture only.

## Run the backend without EdgeDesk

The backend is a normal Unix-socket HTTP server and can be exercised directly.
This is what was executed while writing the example:

```bash
mkdir -p /tmp/edg-notes/run /tmp/edg-notes/data
EDGEDESK_APP_SOCKET=/tmp/edg-notes/run/backend.sock \
EDGEDESK_APP_DATA_DIR=/tmp/edg-notes/data \
  ./backend/notes-backend &

curl --unix-socket /tmp/edg-notes/run/backend.sock http://localhost/health
# {"ok":true,"app":"example"}

curl --unix-socket /tmp/edg-notes/run/backend.sock http://localhost/notes
# {"notes":[]}

curl --unix-socket /tmp/edg-notes/run/backend.sock \
  -H 'Content-Type: application/json' -d '{"text":"hello"}' \
  http://localhost/notes
# {"ok":true}

curl --unix-socket /tmp/edg-notes/run/backend.sock http://localhost/notes
# {"notes":["hello"]}
```

EdgeDesk itself passes the same two variables. Do not listen on TCP and do not
try to derive the socket path from the manifest; the host always supplies it.

## Using it inside EdgeDesk

1. Install the ZIP, approve the app, then open **Backend** settings.
2. Configure a run user/group (root only if you explicitly accept host-level
   access) and start the backend.
3. Open the app window. The UI calls `GET /health` and `GET /notes` through the
   host bridge; `POST /notes` stores a note in the app data directory.

The backend is stopped after install and after upgrade. Autostart is off by
default. There is no automatic rollback of a failed native binary upgrade.

## Not verified here

- ARM64 and ARM32 builds were not executed. Cross-compile commands are in the
  template README; compilation success would not replace on-device testing.
- Installation, Supervisor lifecycle and the `backend.request` proxy are host
  workstreams and depend on their own tests. This example only documents the
  package/SDK side.
