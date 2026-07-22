# Baa-LSP

<p align="center">
  <img src="assets/branding/icons/baa-lsp-256.png" width="160" alt="شعار خادم لغة باء">
</p>

`Baa-LSP` is the official Language Server Protocol adapter for the Baa
programming language and Qalam IDE.

The server does not implement a second Baa parser or semantic analyzer. Baa's
C reference compiler remains the source of language truth. The first server
slice translates versioned LSP document state into the compiler's structured
unsaved-source diagnostic contract:

```text
Qalam or another LSP client
          ↕ LSP over stdio
        Baa-LSP
          ↕ compiler-cli-v1 + diagnostics-json-v1
          Baa
```

## Current slice

- LSP `Content-Length` framing over standard input/output.
- JSON-RPC 2.0 request, response, notification, and error handling.
- `initialize`, `initialized`, `shutdown`, and `exit` lifecycle.
- Full document synchronization through `didOpen`, `didChange`, `didSave`,
  and `didClose`.
- Live diagnostics through `baa --check --diagnostics=json
  --source-stdin=<logical-path>`.
- Conversion from Baa UTF-8 byte spans to LSP UTF-16 positions.
- Version checks that reject stale document changes and analysis results.

## Build

```powershell
cmake -S . -B build -DBAA_LSP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The server has no graphical toolkit dependency. Its production code uses C++23,
the operating system process APIs, and the header-only nlohmann/json library.
Python is used only by the process-level protocol test.

Run the server over standard input/output:

```powershell
baa-lsp --baa-path C:\path\to\baa.exe
```

The `BAA` environment variable and an executable named `baa` on `PATH` are
also supported.

See [the architecture](docs/ARCHITECTURE_AR.md) and [roadmap](ROADMAP.md).

Brand sources and generated icon sizes are documented in
[the branding guide](docs/BRANDING.md).
