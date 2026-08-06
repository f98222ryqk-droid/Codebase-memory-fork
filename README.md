# Codebase-memory-fork

This repository is a fork of [**codebase-memory-mcp**](https://github.com/DeusData/codebase-memory-mcp) — a code intelligence engine for AI coding agents that full-indexes repositories and produces a persistent knowledge graph, exposed through an MCP server.

The project source lives in [`codebase-memory-mcp/`](codebase-memory-mcp/).

## Project layout

```
codebase-memory-mcp/
├── src/          Core C implementation (foundation, store, cypher, daemon,
│                 discover, mcp, pipeline, semantic, ui, cli, git, watcher…)
├── internal/cbm/ Tree-sitter grammars (159 langs), extraction, LSP, C++ preprocessor
├── tests/        Test suite — C unit/integration tests, shell/Python contracts,
│                 repro cases, and Windows-specific tests
├── docs/         Documentation and the UI website
├── graph-ui/     TypeScript/React 3D graph-visualization UI (Vite)
├── pkg/          Distribution packaging (homebrew, npm, pypi, go, winget, aur…)
├── scripts/      Build, CI, lint, security, and release tooling
├── tools/        tree-sitter grammar tooling
├── vendored/     Pinned third-party dependencies (mimalloc, sqlite3, tre, yyjson…)
└── .github/      Issue templates and 20+ CI workflows
```

## Building

See [`codebase-memory-mcp/README.md`](codebase-memory-mcp/README.md) and
`codebase-memory-mcp/Makefile.cbm`:

```sh
cd codebase-memory-mcp
make -f Makefile.cbm cbm     # production binary
make -f Makefile.cbm test    # build + run the test suite (ASan + UBSan)
```
