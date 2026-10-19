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

## zlib

- Upstream: https://zlib.net/
- Source release: https://zlib.net/fossils/zlib-1.3.1.tar.gz
- Version: **1.3.1**
- SHA-256 of `zlib-1.3.1.tar.gz`:
  `9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23`
- License: zlib (see `zlib/LICENSE`; upstream `zlib/README` retained)
- Used by `src/extensions/zip_reader.cpp` (raw DEFLATE inflate and crc32) and
  `tests/extensions/zip_builder.hpp` (deflate/crc32).
- Vendored files are the unmodified upstream C sources and headers required by
  the build. `CMakeLists.txt` compiles them into the static target
  `edgedesk_zlib`, linked `PUBLIC` from `edgedesk_core` so `<zlib.h>` also
  propagates to tests.
- `find_package(ZLIB)` is intentionally not used: Bootlin musl stable-2026.08
  toolchains ship only `lib/libz.so.1*` (no `zlib.h`, `libz.so` or `libz.a`),
  and host lookup could otherwise leak glibc zlib into cross builds.
- Update only by replacing the vendored files with a pinned upstream release
  and updating the version/checksum above.

## sqlite

- Upstream: https://www.sqlite.org/download.html
- Version: 3.54.1 (`sqlite-amalgamation-3540100.zip`)
- License: public domain
- Used only by `src/extensions/` for extension metadata and application KV
  (ADR-0011 local scope exception). Loadable extensions are disabled.
- Details and checksums: `sqlite/README.md`.
