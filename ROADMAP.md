# Baa-LSP Roadmap

## 0.1 — Protocol and diagnostics

- [x] Define the Baa-only ownership boundary.
- [x] Implement bounded `Content-Length` framing.
- [x] Implement JSON-RPC and the LSP process lifecycle.
- [x] Track versioned full-text documents.
- [x] Bridge live diagnostics through Baa's unsaved-source contract.
- [x] Convert UTF-8 byte spans to UTF-16 LSP ranges.
- [x] Complete process-level protocol fixtures on Windows and Linux, including
  real Baa contracts, Takween project navigation, Arabic paths, and active
  compiler-process cancellation/recovery.
- [x] Connect Qalam and remove its direct compiler-analysis transport.

## 0.2 — Symbols and completion

- [x] Add a versioned Baa document-symbol contract.
- [x] Implement `textDocument/documentSymbol` with hierarchical UTF-16 ranges.
- [x] Add context-aware Baa completion and completion resolve.
  - [x] Load `completion-data-json-v1` from Baa and cache it for the server lifetime.
  - [x] Implement Arabic-prefix `textDocument/completion` with UTF-16 edits, snippets, document-global symbols, cancellation, and stale-version rejection.
  - [x] Add compiler-owned visible locals, parameters, included declarations,
    builtin signatures, lexical shadowing, and `completionItem/resolve`
    documentation without adding a parser or semantic table to Baa-LSP.
- [x] Implement compiler-backed hover and signature help with UTF-16 ranges,
  active parameters, cancellation, version checks, and incomplete-call coverage.
- [x] Add cached `workspace/symbol` indexing from Takween's source closure and
  Baa's `symbols-json-v1`, including unsaved-document overlays, cancellation,
  deterministic results, and visible partial-index failures.
- [x] Add full semantic tokens from Baa's `tokens-json-v1`, including strict
  contract validation, multiline splitting, UTF-8-to-UTF-16 conversion,
  caching, cancellation, and stale-version rejection without a second lexer.
- [x] Enrich raw tokens with compiler-bound `semantic-index-json-v1`
  occurrences for functions, variables, parameters, fields, enum members, and
  type declarations, while retaining lexical results for incomplete buffers.

## 0.3 — Navigation and safe editing

- [x] Go to compiler-resolved definitions in the analyzed translation unit,
  including included headers and UTF-16 path/range conversion.
- [x] References with scope-correct translation-unit results, declaration
  filtering, cancellation, and stale-version rejection.
- [x] Expand definition/reference fan-out across Takween workspace files
  without duplicating Baa semantics.
- [x] Collision-checked Arabic rename with versioned workspace edits,
  incomplete-index refusal, and exact compiler-owned occurrences.
- [x] Code actions driven by stable diagnostic codes and structured edits.
- [x] Document formatting through Baa-owned `format-json-v1`, with one
  full-document UTF-16 edit, cancellation, and stale-version rejection.

## 0.4 — Production admission

- [ ] Incremental document overlays or an in-process Baa frontend API.
- [x] Cancellation across LSP, Baa-LSP, and Baa analysis, with active compiler
  process termination and immediate worker recovery proven on Windows/Linux.
- [ ] Workspace folders and Takween project context.
  - [x] Load the initial project's exact source/include closure from
    `takween-build-plan-v1`.
  - [ ] Refresh changed manifests and support multiple dynamic workspace folders.
- [x] Full semantic tokens for compiler-owned lexical and identifier roles.
- [x] Folding and selection ranges through compiler-owned
  `structure-json-v1`, with strict validation, shared per-version caching,
  cancellation, stale-result rejection, and UTF-16 conversion.
- [ ] Compiler-owned inlay hints.
- [x] Qalam-managed crash recovery with capped backoff, unsaved-document reopen,
  a stable-service reset window, and a three-attempt restart limit.
- [ ] Telemetry-free structured server logs.
- [ ] Windows/Linux packaging with Qalam and independent client smoke tests.
