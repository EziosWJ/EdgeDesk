# Vendored dependencies

These sources are pinned and vendored so `host-offline-tests`, musl static-pie
and ARM cross builds do not require network access at configure time.

## argon2

- Upstream: https://github.com/P-H-C/phc-winner-argon2
- Tag: `20190702`
- Commit: `62358ba2123abd17fccf2a108a301d4b52c01a7c`
- License: CC0 1.0 / Apache-2.0 (see `argon2/LICENSE`)
- Used files: reference implementation (`ref.c`), BLAKE2b and public header.
- Build flags: `ARGON2_NO_THREADS`; EdgeDesk serializes hash jobs with its own
  bounded worker pool, so Argon2 must not spawn additional threads.

## nlohmann/json

- Upstream: https://github.com/nlohmann/json
- Version: 3.12.0 (the copy shipped by libhv v1.3.4 under `cpputil/json.hpp`)
- License: MIT (SPDX header is retained in `json.hpp`)
- Used for small schema-versioned state files (`auth.json`, `settings.json`)
  and the extension platform's manifest/KV/integrity metadata.

## zlib (system dependency, not vendored)

- Upstream: https://zlib.net/
- License: zlib
- Used by `src/extensions/zip_reader.cpp` for raw DEFLATE extraction.
- `CMakeLists.txt` uses `find_package(ZLIB REQUIRED)`; host builds use the
  distribution package. Static musl/ARM cross builds must provide `libz.a` in
  the selected toolchain/sysroot. This impact has not yet been verified on the
  L2 matrix; see `docs/testing.md`.

## sqlite

- Upstream: https://www.sqlite.org/download.html
- Version: 3.54.1 (`sqlite-amalgamation-3540100.zip`)
- License: public domain
- Used only by `src/extensions/` for extension metadata and application KV
  (ADR-0011 local scope exception). Loadable extensions are disabled.
- Details and checksums: `sqlite/README.md`.
