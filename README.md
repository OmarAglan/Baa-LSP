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
          ↕ compiler-cli-v1 + diagnostics-json-v1 + symbols-json-v1
            + tokens-json-v1
            + completion-data-json-v1 + semantic-query-json-v1
            + semantic-index-json-v1 + format-json-v1
          Baa
        ↕ takween-build-plan-v1
       Takween
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
- Full `textDocument/semanticTokens/full` results sourced from Baa's
  `tokens-json-v1` raw source contract and compiler-bound
  `semantic-index-json-v1` occurrences. The server validates the exact source
  byte length and identifier spans, converts UTF-8 byte spans to UTF-16, splits
  multiline tokens as LSP requires, and colors functions, variables,
  parameters, fields, enum members, and types without parsing identifier text.
  A source-error exit from the semantic index retains the lexical layer for an
  incomplete buffer; other contract failures remain visible. Only the matching
  document version is cached, and the server does not maintain a second lexer.
- Hierarchical `textDocument/documentSymbol` results sourced from Baa's
  `symbols-json-v1` contract, including types and exact Arabic name ranges.
- Cached `workspace/symbol` results sourced from the same compiler contract.
  Takween supplies the authoritative project source closure; open files use
  their current unsaved text and closed files are read from disk. The server
  filters the cached index locally while the user types and does not parse the
  manifest, compiler messages, or source text.
- LSP request cancellation and content-modified rejection for obsolete symbol
  work.
- Arabic-first `textDocument/completion`: the server loads Baa's versioned
  keywords, directives, snippets, and compiler builtin signatures once, then
  asks `semantic-query-json-v1` for parameters, visible locals, globals, types,
  and explicitly included declarations at the exact cursor. Inner declarations
  shadow outer ones; future and sibling-scope declarations are excluded.
  Results use exact UTF-16 replacement edits, cancellation, and stale-version
  rejection.
- `completionItem/resolve` returns Baa-owned Arabic documentation without
  introducing a language-description table in the server.
- Arabic letters and `#` are advertised as completion triggers; no Latin
  shortcut vocabulary is embedded in the server.
- Compiler-backed `textDocument/hover` and `textDocument/signatureHelp` through
  `semantic-query-json-v1`, with exact UTF-16 ranges, active-parameter
  selection, included prototypes, scope-correct shadowing, cancellation, and
  stale-version rejection.
- Compiler-backed `textDocument/definition` and `textDocument/references`
  through the same cached query, with Arabic/space path URIs, included-header
  locations, declaration filtering, cancellation, and stale-version rejection.
- Takween-aware project fan-out: the server asks Takween for the authoritative
  source/include closure and matches only Baa-owned structured symbol identities.
- `textDocument/prepareRename` and `textDocument/rename` return versioned,
  deduplicated workspace edits for compiler-resolved occurrences. Rename accepts
  Arabic identifiers only and refuses reserved words, compiler-indexed
  collisions, stale documents, or an incomplete project index.
- `textDocument/codeAction` exposes only compiler-owned, explicitly safe quick
  fixes carried by `diagnostics-json-v1`. The server converts exact byte spans,
  binds edits to the current document version, and rejects stale, destructive,
  duplicate, or out-of-document changes without parsing diagnostic messages.
- `textDocument/formatting` asks Baa for `format-json-v1`, rejects stale or
  cancelled work, and returns either no edits for canonical input or one exact
  full-document UTF-16 replacement. The server does not own formatting rules
  and never falls back to an editor-side formatter.

## Build

The recommended Windows command configures, builds, and tests with one selected
MinGW toolchain and a normalized child-process environment:

```powershell
.\scripts\build-windows.ps1
```

The portable cross-platform commands remain:

```powershell
cmake -S . -B build -DBAA_LSP_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The server has no graphical toolkit dependency. Its production code uses C++23,
the operating system process APIs, and the header-only nlohmann/json library.
Python is used only by the process-level protocol test.
On Windows, MinGW and MSVC runtimes are linked into the server and its native
test helpers. CTest also normalizes duplicate `Path`/`PATH` environments for
child processes. The server and tests therefore do not depend on manual DLL
path setup or on whichever compiler toolchain happens to appear first on the
caller's path.

Run the server over standard input/output:

```powershell
baa-lsp --baa-path C:\path\to\baa.exe --takween-path C:\path\to\takween.exe
```

The `BAA` environment variable and an executable named `baa` on `PATH` are
also supported.

See [the architecture](docs/ARCHITECTURE_AR.md) and [roadmap](ROADMAP.md).

Brand sources and generated icon sizes are documented in
[the branding guide](docs/BRANDING.md).
