<p align="center">
  <img src="assets/branding/baa-language-logo.png" width="150" alt="Baa programming language logo">
</p>

<h1 align="center">Baa-LSP</h1>

<p align="center">
  The official Language Server Protocol implementation for the Baa programming language.
</p>

Baa-LSP connects Baa-aware editors—principally [Qalam IDE](https://github.com/OmarAglan/Qalam-IDE)—to the Baa compiler and Takween build system. It is a headless C++23 server that communicates over standard input and output; it has no Qt or other graphical-toolkit dependency.

## Design

Baa-LSP deliberately does not contain a second Baa parser or semantic analyzer:

- **Baa** owns syntax, semantics, diagnostics, formatting, and source structure.
- **Takween** owns project discovery and build context.
- **Baa-LSP** validates their versioned JSON contracts and translates them to LSP.
- **Qalam** presents the resulting language experience.

This boundary keeps command-line builds, editor feedback, and future tooling consistent.

## Language support

The current server provides:

- live diagnostics and safe compiler-provided quick fixes;
- Arabic-first completion, hover, and signature help;
- document and workspace symbols;
- definitions, references, and collision-checked Arabic rename;
- semantic highlighting with compiler-resolved identifier roles;
- document formatting;
- folding and semantic selection ranges; and
- version checks, cancellation, stale-result rejection, and UTF-8/UTF-16 position conversion.

See the [roadmap](ROADMAP.md) for delivery status and [architecture](docs/ARCHITECTURE_AR.md) for the contract and ownership model.

## Build

### Windows

The supported Windows script selects one MinGW toolchain, configures the project, builds the server, and runs its tests:

```powershell
.\scripts\build-windows.ps1
```

### Linux and other CMake environments

```sh
cmake -S . -B build -DBAA_LSP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The build fetches the pinned header-only `nlohmann/json` dependency. Python is
required only for process-level protocol tests. Hosted CI additionally builds
the pinned Baa, Nazm, and Takween revisions and makes the real compiler and
project-navigation suites mandatory on both Windows and Linux.

## Install and package

```sh
cmake --install build --prefix dist/baa-lsp
cmake --build build --target package
```

The install tree places the headless server under `bin/` and its documentation
under `share/`. CPack produces a versioned ZIP on Windows and `.tar.gz` on
Linux. Hosted CI runs the full independent Python protocol client against the
installed binary—not the build-tree executable—before publishing either
archive. Baa and Takween remain separately discoverable ecosystem tools; the
server package does not duplicate their files.

Hosted packaging is verified by
[CI run 31509393734](https://github.com/OmarAglan/Baa-LSP/actions/runs/31509393734),
which publishes `Baa-LSP-Windows-x86_64` and `Baa-LSP-Linux-x86_64` after the
installed-binary smoke passes. Qalam's matching combined artifacts are verified
by [run 31509433467](https://github.com/OmarAglan/Qalam-IDE/actions/runs/31509433467).

## Run

```text
baa-lsp --baa-path <path-to-baa> --takween-path <path-to-takween>
```

If `--baa-path` is omitted, the server also checks the `BAA` environment variable and then `PATH`. LSP messages use `Content-Length` framing over standard input and output, so ordinary log text must never be written to stdout.

## Documentation

- [Architecture and ownership model](docs/ARCHITECTURE_AR.md)
- [Roadmap](ROADMAP.md)
- [Branding assets](docs/BRANDING.md)

The implementation is under active development alongside Baa, Takween, and Qalam. Contract compatibility is tested across the ecosystem before a feature is considered complete.
