# SQLite amalgamation

- Version: **3.54.1** (`sqlite-amalgamation-3540100.zip`)
- Upstream: https://www.sqlite.org/download.html
- Files: `sqlite3.c`, `sqlite3.h`, `sqlite3ext.h`
- License: public domain (SQLite blessing; see the header of `sqlite3.c`)
- SHA-256:
  - `sqlite3.c` `0c657666d819f261cc4007526ad57e1669de9949df7a8ea847aa284a2bf0ee52`
  - `sqlite3.h` `d6c84557261893439b341d2e6c35187ae3ea24fbfd843ad94616f72d89242b8c`

## Why this dependency

ADR-0011 restricts SQLite to extension metadata and application KV. The
amalgamation is compiled directly into `edgedesk_core`; no system
`libsqlite3` is required, which keeps host, musl static-pie and ARM cross
builds reproducible.

Build flags (see `CMakeLists.txt`): `SQLITE_THREADSAFE=1`,
`SQLITE_OMIT_LOAD_EXTENSION=1`, `SQLITE_DQS=0`,
`SQLITE_DEFAULT_FOREIGN_KEYS=1`, `SQLITE_USE_URI=0`,
`SQLITE_ENABLE_JSON1=1`.

## Platform considerations

- Plain C99; cross-compiles with the same GCC/musl toolchains used for the
  rest of EdgeDesk.
- `SQLITE_THREADSAFE=1` uses pthreads/Threads::Threads.
- WAL mode requires shared-memory mapping and a writable directory; the apps
  root is created `0700` and the database `0600`.
- Loadable extensions are disabled; EdgeDesk never loads SQLite plugins.
- The amalgamation is not modified. Update only by replacing all three files
  with a pinned upstream release and updating the checksums/version here.
