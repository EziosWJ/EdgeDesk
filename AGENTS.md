# EdgeDesk Coding Agent Operating Rules

Read this file **before changing code**, then read the relevant file in `docs/spec` and `docs/adr`.

## Single source of truth

- `docs/spec/` = confirmed user-visible behavior; `docs/adr/` = accepted technical decisions or proposals, distinguish status.
- `docs/phase1.md` = current implementation and tests. Do not treat prototype screenshots as proof of completed features.
- React is only an **implementation choice for the Phase 1 bootstrap**; its permanent adoption, UI toolkit, authentication system, and Shell lifecycle beyond approved requirements must be confirmed before declaring product decisions.

## Fixed technology boundaries

- Backend: C++17, libhv v1.3.4 pinned, GCC 15+ Linux release; musl static link is a target and requires a matching toolchain.
- Resource mode: GCC 15 `#embed` in Release, disk mode for local development. Never replace it with huge generated `0xNN` C++ arrays.
- Frontend Phase 1: React + TypeScript + Vite, using `web/public/assets` and tokens from `web/src/styles/edgedesk-tokens.css`.
- No database, ORM, Node runtime on the device, or unnecessary third-party framework.

## Directory ownership

| Directory | Ownership |
| --- | --- |
| `src/app/config.*` | CLI config and validation; no libhv types |
| `src/app/asset_provider.*` | Read-only static frontend resources; no HTTP types |
| `src/app/http_server.*` | libhv routes and transport adapter; no Linux business logic |
| `src/main.cpp` | entry point only |
| `tools/` | build-time generators and tests |
| `web/src/` | React UI; must not call unsafe system commands |
| `web/public/assets/` | source SVG assets; never edit generated dist in Git |
| `docs/spec/`, `docs/adr/` | contracts / decisions |
| `tests/` | deterministic native tests |

Future file operations belong in `FileService` and its OS adapter; PTY sessions in `TerminalSessionManager`; metrics in `HardwareMonitorService`. Route handlers only adapt protocol input/output.

## Mandatory coding constraints

1. Keep changes focused; never silently rewrite confirmed APIs or decisions.
2. No global mutable business state, naked owning new/delete, or infinite unchecked queues/threads.
3. Check all system call return values; use RAII for FD, PTY, socket and process lifecycle.
4. All externally supplied paths must be validated. Future privileged file operations require directory-FD/path-race-safe design, not a `realpath`-only check.
5. PTY Bash process must survive switching tabs, route changes and reload, independently of one WebSocket lifetime. Do not substitute `/bin/sh` for Bash or assume `.bashrc` is always loaded.
6. **No public bind / credentials bypass:** Phase 1 binds loopback only. Do not expose 0.0.0.0 or add privileged endpoints before authentication, authorization, CSWSH protection, and transport security have been designed and tested.
7. Never block the libhv event loop with unbounded filesystem, process or monitoring work.
8. Keep static resource routing and device file management strictly separate.
9. Distinguish host builds, cross-compilation and physical device tests; only claim what actually ran.
10. New dependencies require justification, version pinning, license check and ARM/musl impact analysis.

## Completion criteria for every task

- Run relevant test/build commands, report exact successes and failures.
- Update docs for changed behavior or API. Do not fabricate screenshots, benchmarks, or hardware test claims.
- Keep generated `out/`, `web/dist/`, `web/node_modules/` out of Git.
- Phase 1 API contract: `GET /api/health` => HTTP 200 JSON with `status=ok`, `service=edgedesk`, `phase=1`.
- All later features should have issue-specific acceptance tests before marking complete.
