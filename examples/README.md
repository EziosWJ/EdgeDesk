# EdgeDesk examples

| Example | Description |
| --- | --- |
| [`native-backend-notes`](native-backend-notes/) | Static Web UI plus a POSIX C backend served over the per-application Unix domain socket (SPEC-0011). |

Each example is a complete package directory. Build its backend (if any),
then run the SDK CLI `validate` and `pack` commands from
[`tools/ext-sdk`](../tools/ext-sdk/).

Native backends have no scripting runtime: the package ships one ELF
executable per architecture, and EdgeDesk never executes package content during
installation.
