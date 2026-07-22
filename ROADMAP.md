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

- [ ] Add a versioned Baa document-symbol contract.
- [ ] Implement `textDocument/documentSymbol`.
- [ ] Add context-aware Baa completion and completion resolve.
- [ ] Implement hover and signature help.
- [ ] Add workspace symbol indexing without duplicating Baa semantics.

## 0.3 — Navigation and safe editing

- [ ] Go to definition and declaration.
- [ ] References with scope-correct results.
- [ ] Collision-checked rename with workspace edits.
- [ ] Code actions driven by stable diagnostic codes and structured edits.
- [ ] Document formatting after Baa owns a stable formatting contract.

## 0.4 — Production admission

- [ ] Incremental document overlays or an in-process Baa frontend API.
- [ ] Cancellation across LSP, Baa-LSP, and Baa analysis.
- [ ] Workspace folders and Takween project context.
- [ ] Semantic tokens, folding ranges, selection ranges, and inlay hints.
- [ ] Crash recovery, restart limits, telemetry-free structured logs.
- [ ] Windows/Linux packaging with Qalam and independent client smoke tests.
