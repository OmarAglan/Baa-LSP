# Baa-LSP Roadmap

## 0.1 — Protocol and diagnostics

- [x] Define the Baa-only ownership boundary.
- [x] Implement bounded `Content-Length` framing.
- [x] Implement JSON-RPC and the LSP process lifecycle.
- [x] Track versioned full-text documents.
- [x] Bridge live diagnostics through Baa's unsaved-source contract.
- [x] Convert UTF-8 byte spans to UTF-16 LSP ranges.
- [ ] Complete process-level protocol fixtures on Windows and Linux.
- [x] Connect Qalam and remove its direct compiler-analysis transport.

## 0.2 — Symbols and completion

- [x] Add a versioned Baa document-symbol contract.
- [x] Implement `textDocument/documentSymbol` with hierarchical UTF-16 ranges.
- [ ] Add context-aware Baa completion and completion resolve.
  - [x] Load `completion-data-json-v1` from Baa and cache it for the server lifetime.
  - [x] Implement Arabic-prefix `textDocument/completion` with UTF-16 edits, snippets, document-global symbols, cancellation, and stale-version rejection.
  - [ ] Add scope-aware locals, included standard-library declarations, compiler builtins, and completion resolve documentation.
- [x] Implement compiler-backed hover and signature help with UTF-16 ranges,
  active parameters, cancellation, version checks, and incomplete-call coverage.
- [ ] Add workspace symbol indexing without duplicating Baa semantics.

## 0.3 — Navigation and safe editing

- [x] Go to compiler-resolved definitions in the analyzed translation unit,
  including included headers and UTF-16 path/range conversion.
- [x] References with scope-correct translation-unit results, declaration
  filtering, cancellation, and stale-version rejection.
- [x] Expand definition/reference fan-out across Takween workspace files
  without duplicating Baa semantics.
- [x] Collision-checked Arabic rename with versioned workspace edits,
  incomplete-index refusal, and exact compiler-owned occurrences.
- [ ] Code actions driven by stable diagnostic codes and structured edits.
- [ ] Document formatting after Baa owns a stable formatting contract.

## 0.4 — Production admission

- [ ] Incremental document overlays or an in-process Baa frontend API.
- [ ] Cancellation across LSP, Baa-LSP, and Baa analysis.
- [ ] Workspace folders and Takween project context.
  - [x] Load the initial project's exact source/include closure from
    `takween-build-plan-v1`.
  - [ ] Refresh changed manifests and support multiple dynamic workspace folders.
- [ ] Semantic tokens, folding ranges, selection ranges, and inlay hints.
- [ ] Crash recovery, restart limits, telemetry-free structured logs.
- [ ] Windows/Linux packaging with Qalam and independent client smoke tests.
