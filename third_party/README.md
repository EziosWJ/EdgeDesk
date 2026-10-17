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
- Used by `src/security` only for small, schema-versioned state files
  (`auth.json`, `settings.json`). It is not exposed through public headers.
